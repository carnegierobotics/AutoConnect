/**
 * @file: AutoConnect/include/AutoConnect/AutoConnectLinux.h
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
 *   2022-7-14, mgjerde@carnegierobotics.com, Created file.
 **/

#ifndef AUTOCONNECT_AUTOCONNECTLINUX_H
#define AUTOCONNECT_AUTOCONNECTLINUX_H


#include <thread>
#include <mutex>
#include <cstdarg>
#include <sstream>
#include <AutoConnect/Json.hpp>
#include <semaphore.h>

#include "AutoConnect/ThreadPool.h"

#define NUM_WORKER_THREADS 5


class AutoConnectLinux {

public:
    struct Adapter {
        Adapter() = default;
        explicit Adapter(const char *name, uint32_t index) : ifName(name),
                                                             ifIndex(index) { // By default, we want to initialize an adapter result with a name and an index
        }

        bool supports = true;
        bool available = true;
        bool checkingForCamera = false;
        std::vector<std::string> IPAddresses;
        std::vector<std::string> searchedIPs;
        std::string description;
        std::string ifName;
        uint32_t ifIndex = 0;
        std::vector<std::string> cameraIPAddresses;
        std::vector<std::string> cameraNameList;

        bool isSearched(const std::string &ip) {
            for (const auto &searched: searchedIPs) {
                if (searched == ip)
                    return true;
            }
            return false;
        }

        nlohmann::json sendAdapterResult() {
            nlohmann::json j;
            j["Name"] = ifName;
            j["Index"] = ifIndex;
            j["Description"] = description;
            j["AddressList"] = cameraIPAddresses;
            j["CameraNameList"] = cameraNameList;

            return j;
        }
    };

    ~AutoConnectLinux() = default;

    explicit AutoConnectLinux(bool enableIPC, bool logToConsole = false) {
        out = {
                {"Name", "AutoConnect"},
                {"Version", "v1.0.0"},
                {"Log",  {""}}
        };

        if (logToConsole)
            m_LogToConsole = true;

        m_Pool = std::make_unique<AutoConnect::ThreadPool>(NUM_WORKER_THREADS);
        m_IsRunning = true;
        log("Started AutoConnect service");

        m_Pool->Push(AutoConnectLinux::adapterScan, this);
        m_Pool->Push(AutoConnectLinux::runInternal, this, enableIPC);

    }

    [[nodiscard]] bool pollEvents() const {

        return m_IsRunning;
    }

    bool m_LogToConsole = false;

    /**
     * Pushes string to message queue. Mutex protected.
     * Adds a newline character to each message
     * @param msg message to push onto queue
     */
    template<typename ...Args>
    void log(Args &&...args) {
        std::ostringstream stream;
        (stream << ... << std::forward<Args>(args)) << '\n';

        std::scoped_lock<std::mutex> lock(m_logQueueMutex);
        if (out.contains("Log"))
            out["Log"].emplace_back(stream.str());

        if (m_LogToConsole)
            std::cout << stream.str() << std::flush;
    }

    void notifyStop() {
        std::scoped_lock<std::mutex> lock(m_logQueueMutex);

        out["Command"] = "Stop";
        if (m_LogToConsole)
            std::cout << "notifyStop: " << "Stop" << std::endl;
    }

    static void adapterScan(void *ctx);

    static void listenOnAdapter(void *ctx, Adapter *adapter);

    static void checkForCamera(void *ctx, Adapter *adapter);

    void cleanUp();

private:
    nlohmann::json out;

    std::unique_ptr<AutoConnect::ThreadPool> m_Pool;
    std::vector<Adapter> m_Adapters;
    std::mutex m_AdaptersMutex;
    std::mutex m_logQueueMutex;
    bool m_IsRunning = false;
    bool m_ListenOnAdapter = true;
    bool m_ScanAdapters = true;


    static void run(void *instance);

    static void runInternal(void *ctx, bool enableIPC);

    void reportAndExit(const char *msg);

    void sendMessage(caddr_t memPtr, sem_t *semPtr);

    void getMessage(caddr_t memPtr, sem_t *semPtr);

    void setAddress();

    void setHostAddress(const std::string &adapterName, const std::string &hostAddress);

    void setMTU(const std::string &adapterName, int mtu = 7200);
};


#endif //AUTOCONNECT_AUTOCONNECTLINUX_H
