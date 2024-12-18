/**
 * @file: AutoConnect/src/AutoConnectLinux.cpp
 *
 * Copyright 2022
 * Carnegie Robotics, LLC
 * 4501 Hatfield Street, Pittsburgh, PA 15201
 * http://www.carnegierobotics.com
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the Carnegie Robotics, LLC nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL CARNEGIE ROBOTICS, LLC BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Significant history (date, user, action):
 *   2022-09-12, mgjerde@carnegierobotics.com, Created file.
 **/
//
// Created by magnus on 7/14/22.
//

#include <cstring>
#include <linux/sockios.h>
#include <net/if.h>
#include <linux/ethtool.h>
#include <netinet/ip.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <netpacket/packet.h>
#include <mutex>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

#include <sys/stat.h>
#include <MultiSense/MultiSenseChannel.hh>

#include "AutoConnect/AutoConnectLinux.h"

#define ByteSize 65536
#define BackingFile "/mem"
#define AccessPerms 0777
#define SemaphoreName "sem"

void AutoConnectLinux::reportAndExit(const char *msg) {
    log("%s ", msg);
    m_IsRunning = false;
}

void AutoConnectLinux::sendMessage(caddr_t memPtr, sem_t *semPtr) {
    std::scoped_lock<std::mutex> lock(m_logQueueMutex);
    if (semPtr == (void *) -1)
        reportAndExit("sem_open");
    strcpy(memPtr, to_string(out).c_str());
    if (sem_post(semPtr) < 0)
        reportAndExit("sem_post");
}

void AutoConnectLinux::getMessage(caddr_t memPtr, sem_t *semPtr) {
    if (semPtr == (void *) -1)
        reportAndExit("sem_open");
    std::string str(memPtr + (ByteSize / 2));
    memset(memPtr + (ByteSize / 2), 0x00, ByteSize / 2);
    if (sem_post(semPtr) < 0)
        reportAndExit("sem_post");

    if (!str.empty()) {
        auto json = nlohmann::json::parse(str);
        std::cout << json.dump(4) << std::endl;
        if (json.contains("Command")) {
            if (json["Command"] == "Stop") {
                log("Stopping auto connect");
                sendMessage(memPtr, semPtr);
                cleanUp();
            }

        }
        if (json.contains("SetIP")) {
            log("Setting ip");
            std::string indexStr = json["index"];
            log("index str: " + indexStr);
            int index = 0;
            try {
                index = std::stoi(indexStr);
                // Use the 'index' variable here
                nlohmann::json res = out["Result"];
                std::string interfaceName = res[index]["Name"];
                std::string ip = res[index]["AddressList"][0];
                // Set the host ip address to the same subnet but with *.2 at the end.
                std::string hostAddress = ip;
                std::string last_element(hostAddress.substr(hostAddress.rfind(".")));
                auto ptr = hostAddress.rfind('.');
                hostAddress.replace(ptr, last_element.length(), ".2");
                log("Setting ip: " + hostAddress + " At interface: " + interfaceName);

                setHostAddress(interfaceName, hostAddress);
                setMTU(interfaceName, 7200);
            } catch (const std::exception &e) {
                // Handle the exception here
                std::cout << "An exception occurred: " << e.what() << std::endl;
            }

        }
    }
}

void AutoConnectLinux::runInternal(void *ctx, bool enableIPC) {
    auto *app = static_cast<AutoConnectLinux *>(ctx);
    auto time = std::chrono::steady_clock::now();

    int fd = -1;
    caddr_t memPtr;
    sem_t *semPtr;
    mode_t old_umask = umask(0);
    if (enableIPC) {
        fd = shm_open(BackingFile,      /* name from smem.h */
                      O_RDWR | O_CREAT, /* read/write, create if needed */
                      AccessPerms);     /* access permissions */
        if (fd < 0) app->reportAndExit("Can't open shared mem segment...");
        int res = ftruncate(fd, ByteSize); /* get the bytes */
        if (res != 0) {
            app->reportAndExit("Failed to get the bytes...");
        }
        memPtr = static_cast<caddr_t>(mmap(NULL,       /* let system pick where to put segment */
                                           ByteSize,   /* how many bytes */
                                           PROT_READ | PROT_WRITE, /* access protections */
                                           MAP_SHARED, /* mapping visible to other processes */
                                           fd,         /* file descriptor */
                                           0));         /* offset: start at 1st byte */

        if ((caddr_t) -1 == memPtr) app->reportAndExit("Can't get segment...");
        semPtr = sem_open(SemaphoreName, /* name */
                          O_CREAT,       /* create the semaphore */
                          AccessPerms,   /* protection perms */
                          0);            /* initial value */

    }

    while (app->m_IsRunning) {
        // Find a list of available adapters
        {
            std::scoped_lock<std::mutex> lock(app->m_AdaptersMutex);
            for (auto &item: app->m_Adapters) {
                if (item.supports && item.available) {
                    item.available = false;
                    app->m_Pool->Push(AutoConnectLinux::listenOnAdapter, app, &item);
                }
            }
        }
        // Add a task to check for cameras on an adapter
        {
            std::scoped_lock<std::mutex> lock(app->m_AdaptersMutex);
            for (auto &item: app->m_Adapters) {
                if (!item.IPAddresses.empty() && !item.checkingForCamera) {
                    app->m_Pool->Push(AutoConnectLinux::checkForCamera, app, &item);
                    item.checkingForCamera = true;
                }
            }
        }
        if (enableIPC)
            app->sendMessage(memPtr, semPtr);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (enableIPC)
            app->getMessage(memPtr, semPtr);
        auto time_span = std::chrono::duration_cast<std::chrono::duration<float>>(
                std::chrono::steady_clock::now() - time);
        if (time_span.count() > 60) {
            app->log("Time limit of 60s reached. Exiting AutoConnect.");
            break;
        }
    }
    app->log("Exiting autoconnect");
    if (enableIPC) {
        app->notifyStop();
        app->sendMessage(memPtr, semPtr);
    }

    if (enableIPC) {
        /* clean up */
        munmap(memPtr, ByteSize); /* unmap the storage */
        close(fd);
        sem_close(semPtr);
        umask(old_umask);
        shm_unlink(BackingFile); /* unlink from the backing file */
    }
    app->m_IsRunning = false;
}

void AutoConnectLinux::adapterScan(void *ctx) {
    auto *app = static_cast<AutoConnectLinux *>(ctx);
    app->log("Performing adapter scan");
    while (app->m_ScanAdapters) {
        // Get list of interfaces
        std::vector<Adapter> adapters;
        auto ifn = if_nameindex();
        // If no interfaces. This turns to null if there is no interfaces for a few seconds
        if (!ifn) {
            app->log("if_nameindex error: ", strerror(errno));
            continue;
        }
        auto fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);


        for (auto i = ifn; i->if_name; ++i) {
            struct {
                __u32 link_mode_data[3 * 127]{};
                struct ethtool_link_settings req{};
            } ecmd{};
            Adapter adapter(i->if_name, i->if_index);

            auto ifr = ifreq{};
            std::strncpy(ifr.ifr_name, i->if_name, IF_NAMESIZE);

            ecmd.req.cmd = ETHTOOL_GLINKSETTINGS;
            ifr.ifr_data = reinterpret_cast<char *>(&ecmd);

            // Check if interface is of type ethernet
            if (ioctl(fd, SIOCETHTOOL, &ifr) == -1) {
                adapter.supports = false;
            }
            // More ethernet checking
            if (ecmd.req.link_mode_masks_nwords >= 0 || ecmd.req.cmd != ETHTOOL_GLINKSETTINGS) {
                adapter.supports = false;
            }
            // Even more ethernet checking
            ecmd.req.link_mode_masks_nwords = -ecmd.req.link_mode_masks_nwords;
            if (ioctl(fd, SIOCETHTOOL, &ifr) == -1) {
                adapter.supports = false;
            }

            adapters.emplace_back(adapter);
        }
        if_freenameindex(ifn);
        close(fd);

        // Put into shared list
        // If the name is new then insert it in the list
        {
            std::scoped_lock<std::mutex> lock(app->m_AdaptersMutex);
            for (const auto &adapter: adapters) {
                bool exist = false;
                for (const auto &shared: app->m_Adapters) {
                    if (shared.ifName == adapter.ifName)
                        exist = true;
                }
                if (!exist) {
                    app->m_Adapters.emplace_back(adapter);
                    app->log("Found adapter: ", adapter.ifName, " index: ", adapter.ifIndex, " supports: ",
                             adapter.supports);

                }
            }
        }
        // Don't update too fast
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

void AutoConnectLinux::listenOnAdapter(void *ctx, Adapter *adapter) {
    auto *app = static_cast<AutoConnectLinux *>(ctx);
    // Submit request for a socket descriptor to look up interface.
    app->log("Configuring adapter: ", adapter->ifName);

    int sd = 0;
    if ((sd = socket(PF_PACKET, SOCK_RAW, htons(ETH_P_IP))) < 0) {
        app->log("socket() Failed to get socket descriptor for using ioctl() ", adapter->ifName, " : ",
                 strerror(errno));
    }

    struct sockaddr_ll addr{};
    // Bind socket to interface
    memset(&addr, 0x00, sizeof(addr));
    addr.sll_family = AF_PACKET;
    addr.sll_protocol = htons(ETH_P_ALL);
    addr.sll_ifindex = (int) adapter->ifIndex;
    if (bind(sd, (struct sockaddr *) &addr, sizeof(addr)) == -1) {
        app->log("Error in bind: ", adapter->ifName, " : ", strerror(errno));
    }
    // set the network card in promiscuos mode//
    // An ioctl() request has encoded in it whether the argument is an in parameter or out parameter
    // SIOCGIFFLAGS	0x8913		// get flags			//
    // SIOCSIFFLAGS	0x8914		// set flags			//
    struct ifreq ethreq{};
    strncpy(ethreq.ifr_name, adapter->ifName.c_str(), IF_NAMESIZE);
    if (ioctl(sd, SIOCGIFFLAGS, &ethreq) == -1) {
        app->log("Error in ioctl get flags: ", adapter->ifName, " : ", strerror(errno));
    }
    ethreq.ifr_flags |= IFF_PROMISC;

    if (ioctl(sd, SIOCSIFFLAGS, &ethreq) == -1) {
        app->log("Error in ioctl set flags: ", adapter->ifName, " : ", strerror(errno));
    }

    int saddr_size, data_size;
    struct sockaddr saddr{};
    auto *buffer = (unsigned char *) malloc(IP_MAXPACKET + 1);
    auto startListenTime = std::chrono::steady_clock::now();
    float timeOut = 15.0f;
    app->log("Performing MultiSense camera search on adapter: ", adapter->ifName);
    while (app->m_ListenOnAdapter) {
        // Timeout handler
        // Will timeout MAX_CONNECTION_ATTEMPTS times until retrying on new adapter
        auto timeSpan = std::chrono::duration_cast<std::chrono::duration<float>>(
                std::chrono::steady_clock::now() - startListenTime);
        if (timeSpan.count() > timeOut)         // x Seconds, then break loop
            break;

        saddr_size = sizeof(saddr);
        //Receive a packet
        data_size = (int) recvfrom(sd, buffer, IP_MAXPACKET, MSG_DONTWAIT, &saddr,
                                   (socklen_t *) &saddr_size);
        if (data_size < 0) {
            continue;
        }

        //Now process the packet
        auto *iph = (struct iphdr *) (buffer + sizeof(struct ethhdr));
        struct in_addr ip_addr{};
        std::string address;
        if (iph->protocol == IPPROTO_IGMP) //Check the Protocol and do accordingly...
        {
            ip_addr.s_addr = iph->saddr;
            address = inet_ntoa(ip_addr);
            // If not already in vector
            std::scoped_lock<std::mutex> lock(app->m_AdaptersMutex);
            // Check if we havent added this ip or searched it before
            if (std::find(adapter->IPAddresses.begin(), adapter->IPAddresses.end(), address) ==
                adapter->IPAddresses.end() &&
                std::find(adapter->searchedIPs.begin(), adapter->searchedIPs.end(), address) ==
                adapter->searchedIPs.end()
                    ) {
                app->log("Got address ", address.c_str(), " On adapter: ", adapter->ifName.c_str());
                adapter->IPAddresses.emplace_back(address);
            }
        }
    }
    free(buffer);
}

void AutoConnectLinux::setHostAddress(const std::string &adapterName, const std::string &hostAddress) {
    int fd = -1;
    if ((fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        log("Failed to create socket: ", adapterName, " : ", strerror(errno));
    }
    // CALL IOCTL Operations to set the address of the adapter/socket  //
    struct ifreq ifr{};
    /// note: no pointer here
    struct sockaddr_in inet_addr{}, subnet_mask{};
    // get interface name //
    // Prepare the struct ifreq //
    bzero(ifr.ifr_name, IFNAMSIZ);
    strncpy(ifr.ifr_name, adapterName.c_str(), IFNAMSIZ);

    /// note: prepare the two struct sockaddr_in
    inet_addr.sin_family = AF_INET;
    inet_pton(AF_INET, hostAddress.c_str(), &(inet_addr.sin_addr));
    subnet_mask.sin_family = AF_INET;
    inet_pton(AF_INET, "255.255.255.0", &(subnet_mask.sin_addr));
    // Call ioctl to configure network devices
    /// put addr in ifr structure
    memcpy(&(ifr.ifr_addr), &inet_addr, sizeof(struct sockaddr));
    int ioctl_result = ioctl(fd, SIOCSIFADDR, &ifr);  // Set IP address
    if (ioctl_result < 0) {
        log("Error in ioctl set address: ", adapterName, " : ", strerror(errno));
    }
    /// put mask in ifr structure
    memcpy(&(ifr.ifr_addr), &subnet_mask, sizeof(struct sockaddr));
    ioctl_result = ioctl(fd, SIOCSIFNETMASK, &ifr);   // Set subnet mask
    if (ioctl_result < 0) {
        log("Error in ioctl set netmask: ", adapterName, " : ", strerror(errno));
    }
}

void AutoConnectLinux::setMTU(const std::string &adapterName, int mtu) {
    int fd = -1;
    if ((fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        log("Failed to create socket: ", adapterName, " : ", strerror(errno));
    }
    // CALL IOCTL Operations t
    // o set the address of the adapter/socket  //
    struct ifreq ifr{};
    // Set MTU
    strncpy(ifr.ifr_name, adapterName.c_str(),
            sizeof(ifr.ifr_name));//interface m_Name where you want to set the MTU
    ifr.ifr_mtu = mtu; //your MTU  here
    if (ioctl(fd, SIOCSIFMTU, (caddr_t) &ifr) < 0) {
        log("Failed to set MTU to 7200 on: ", adapterName);
    } else {
        log("Set MTU to 7200 on: ", adapterName);
    }
}

void AutoConnectLinux::checkForCamera(void *ctx, Adapter *adapter) {
    std::string address;
    std::string adapterName;
    auto *app = static_cast<AutoConnectLinux *>(ctx);
    {
        std::scoped_lock<std::mutex> lock(app->m_AdaptersMutex);
        if (!app->m_IsRunning || !app->m_ListenOnAdapter || !app->m_ScanAdapters)
            return;

        bool searchedAll = true;
        for (const auto &item: adapter->IPAddresses) {
            if (!adapter->isSearched(item)) {
                searchedAll = false;
            }
        }
        if (searchedAll) {
            adapter->checkingForCamera = false;
            return;
        }
        address = adapter->IPAddresses.front();
        adapterName = adapter->ifName;
        adapter->IPAddresses.erase(adapter->IPAddresses.begin());
        app->log("Checking for camera at ", address.c_str(), " on: ", adapter->ifName.c_str());

    }
    // Set the host ip address to the same subnet but with *.2 at the end.
    std::string hostAddress = address;
    std::string last_element(hostAddress.substr(hostAddress.rfind(".")));
    auto ptr = hostAddress.rfind('.');
    hostAddress.replace(ptr, last_element.length(), ".2");
    app->setHostAddress(adapterName, hostAddress);
    // Add a delay to let changes propagate through system
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    auto *channelPtr = crl::multisense::Channel::Create(address, adapterName);
    {
        std::scoped_lock<std::mutex> lock(app->m_AdaptersMutex);
        if (channelPtr != nullptr) {
            app->log("Success. Found a MultiSense device at: ", address.c_str(), " on: ", adapterName.c_str());
            crl::multisense::system::DeviceInfo info;
            channelPtr->getDeviceInfo(info);
            crl::multisense::Channel::Destroy(channelPtr);
            app->setMTU(adapterName, 7200);

            adapter->cameraNameList.emplace_back(info.name);
            adapter->cameraIPAddresses.emplace_back(address);
            {
                std::scoped_lock<std::mutex> lock2(app->m_logQueueMutex);
                app->out["Result"].emplace_back(adapter->sendAdapterResult());
            }
        } else {
            app->log("No camera at ", address);
        }
        adapter->searchedIPs.emplace_back(address);
        adapter->checkingForCamera = false;
    }
}

void AutoConnectLinux::cleanUp() {
    m_IsRunning = false;
    m_ListenOnAdapter = false;
    m_ScanAdapters = false;
}
