/**
 * @file: AutoConnect/include/AutoConnect/WinRegEditor.h
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
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winreg.h>
#include <iostream>
#include <cassert>
#include <vector>
#include <fstream>
#include <iphlpapi.h>
#include <shellapi.h>

#pragma comment(lib,"Advapi32.lib")

// Needed parameters for this winreg to configure static IP address:
// 1. Interface UUID name to: Disable DHCP, Set IP address and subnet mask	: To modify the correct adapter
// 2. IP Address and Subnet mask											: To sync camera and pc to same network
// 3. Interface Description (Lenovo USB Ethernet)							: To set MTU/Enable Jumbo
// 4. Interface Index to													: To restart adapter effectively applying changes

// Needed parameters for this winreg to configure temporary IP address:
//  The IPv4 address exists only as long as the adapter object exists. Restarting the computer destroys the IPv4 address, as does manually resetting the network interface card (NIC).
// To create non-persistent ipv4 address pass into the constructor:
// 1. adapter index
// 2. IP address to configure
// 3. SubNet Mask
class WinRegEditor {


public:
    HKEY tcpIpKey;
    std::vector<HKEY> adapterKeys;
    uint32_t index;
    bool ready = false;
    std::string name;
    std::string adapterDesc;
    HKEY startupKeyRes{};

    // non-persistent IP
    ULONG NTEContext = 0;
    ULONG NTEInstance = 0;

    WinRegEditor(uint32_t ifIndex, std::string ipv4Addr, std::string subnetMask) {

        setStaticIp(ifIndex, ipv4Addr, subnetMask);
    }

    bool setStaticIp(uint32_t ifIndex, std::string ipv4Addr, std::string subnetMask) {
        UINT  iaIPAddress = inet_addr(ipv4Addr.c_str());
        UINT iaIPMask = inet_addr(subnetMask.c_str());

        DWORD dwRetVal = AddIPAddress(iaIPAddress, iaIPMask, ifIndex,
                                      &NTEContext, &NTEInstance);
        if (dwRetVal != NO_ERROR) {
            printf("AddIPAddress call failed with %d\n", dwRetVal);
            return false;
        }

        return true;
    }

    bool deleteStaticIP() {
        DWORD dwRetVal = DeleteIPAddress(NTEContext);
        if (dwRetVal != NO_ERROR) {
            printf("\tDeleteIPAddress failed with error: %d\n", dwRetVal);
        }
    }

    /**@brief
    @param lpkey: suggestion {7a71db7f-b10a-4fa2-8493-30ad4e2a947d}
    @param adapterDescription: Lenovo USB Ethernet
    **/
    WinRegEditor(std::string lpKey, std::string adapterDescription, uint32_t index) {
        // {7a71db7f-b10a-4fa2-8493-30ad4e2a947d}
        this->name = lpKey;
        this->index = index;
        this->adapterDesc = adapterDescription;

        DWORD lResult = RegOpenKeyEx(HKEY_LOCAL_MACHINE, ("SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces\\" + lpKey).c_str(), 0, KEY_READ | KEY_SET_VALUE, &tcpIpKey);
        adapterKeys = findAdapterKey(adapterDescription);
        if (adapterKeys.empty()) {
            std::cout << "Failed to retrieve adapter key\n";
        }
        else {
            ready = true;
        }

        long res = RegCreateKeyA(
                HKEY_LOCAL_MACHINE,
                "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\RunOnce",
                &startupKeyRes
        );

        if (res != ERROR_SUCCESS)
            printf("Something went wrong creating RunOnce Key\n");

        std::string cmd = "\"C:\\Program Files (x86)\\MultiSense Viewer\\Assets\\Tools\\windows\\RegistryBackup.exe\"";

        res = RegSetKeyValueA(
                startupKeyRes,
                NULL,
                "RevertSettings",
                REG_SZ,
                cmd.c_str(),
                cmd.size()
        );

    }

    struct PreChange {
        DWORD EnableDHCP = 1;
        std::string IPAddress = "";
        std::string SubnetMask = "";
        std::string JumboPacket = "";
    }backup;

    void dontLaunchOnReboot() {
        LSTATUS res = RegDeleteKeyValueA(
                startupKeyRes,
                NULL,
                "RevertSettings"
        );

        if (res != ERROR_SUCCESS) {
            printf("Failed to delete key: revert settings on reboot\n");
        }

        RegCloseKey(startupKeyRes);
    }

    void restartNetAdapters() {
        std::string strIdx = std::to_string(index);
        auto shl = ShellExecuteA(0, 0, "powershell.exe", std::string("Get-NetAdapter -InterfaceIndex " + strIdx + " | Restart-NetAdapter").c_str(), "", SW_HIDE);
        if ((int)shl <= 32){
            std::cout << "Failed to restart adapter after jumbo packet configuration" << std::endl;
        }
    }

    int revertSettings() {

        DWORD ret = -1;
        ret = RegSetValueExA(tcpIpKey, "EnableDHCP", 0, REG_DWORD, (const BYTE*)&backup.EnableDHCP, sizeof(DWORD));
        if (ret != 0)
        {
            std::cout << "Failed to reset EnableDHCP\n";
            return 1;
        }

        ret = RegSetValueExA(tcpIpKey, "IPAddress", 0, REG_MULTI_SZ, (const BYTE*)backup.IPAddress.c_str(), backup.IPAddress.size() + 1);

        if (ret != 0) {
            std::cout << "Failed to reset IPADDRESS\n";
            return 1;
        }

        ret = RegSetValueExA(tcpIpKey, "SubnetMask", 0, REG_MULTI_SZ, (const BYTE*)backup.SubnetMask.c_str(), backup.SubnetMask.size() + 1);

        if (ret != 0) {
            std::cout << "Failed to reset SubnetMask\n";
            return 1;
        }
    }

    int setTCPIPValues(std::string ip, std::string subnetMask) {
        // Disable DHCP
        DWORD var = 0;
        DWORD ret = RegSetValueExA(tcpIpKey, "EnableDHCP", 0, REG_DWORD, (const BYTE*)&var, sizeof(DWORD));
        if (ret != ERROR_SUCCESS)
        {
            std::cout << "Failed to Set EnableDHCP to false\n";
            return 1;
        }
        // IPAddress
        //ip.push_back((char) "\0");
        ret = RegSetValueExA(tcpIpKey, "IPAddress", 0, REG_MULTI_SZ, (const BYTE*)ip.c_str(), ip.size());
        if (ret != ERROR_SUCCESS) {
            std::cout << "Failed to Set IPADDRESS\n";
            return 1;
        }
        // SubnetMask
        //subnetMask.push_back((char) "\0");
        ret = RegSetValueExA(tcpIpKey, "SubnetMask", 0, REG_MULTI_SZ, (const BYTE*)subnetMask.c_str(), subnetMask.size());
        if (ret != ERROR_SUCCESS) {
            std::cout << "Failed to Set SubnetMask\n";
            return 1;
        }
    }

    int setJumboPacket(std::string value) {
        // Set *JumboPacket on keys matching description to our netadapter
        for (const auto& key : adapterKeys){
            DWORD ret = RegSetValueExA(key, "*JumboPacket", 0, REG_SZ, (const BYTE*)value.c_str(), value.size());
            if (ret != ERROR_SUCCESS) {
                std::cout << "Failed to Set *JumboPacket\n";
                return -1;
            }
            else {
                std::cout << "Set *JumboPacket on device: " << "Lenovo USB Ethernet" << std::endl;
            }
        }
    }

    int resetJumbo() {
        return setJumboPacket(backup.JumboPacket);
    }

private:
    std::vector<HKEY> findAdapterKey(std::string driverDesc) {
        HKEY queryKey;
        DWORD lResult2 = RegOpenKeyEx(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e972-e325-11ce-bfc1-08002be10318}", 0, KEY_READ, &queryKey);
        // Open the GUID class 4d36e972-e325-11ce-bfc1-08002be10318
        // List the sub keys in this category. I will be looking for devices with iftype = 6
        // Further distinguish devices by looking at the:
        // - DriverDesc
        // - NetCfgInstanceid (hex string)
        // Once found then set the *JumboPacket value

        const DWORD MAX_KEY_LENGTH = 255;
        const DWORD MAX_VALUE_NAME = 255;

        CHAR* achClass = new char[200];
        DWORD    cbName = 0;                   // size of name string
        DWORD    cchClassName = 0;  // size of class string
        DWORD    cSubKeys = 0;               // number of subkeys
        DWORD    cbMaxSubKey = 0;              // longest subkey size
        DWORD    cchMaxClass = 0;              // longest class string
        DWORD    cValues = 0;              // number of values for key
        DWORD    cchMaxValue = 0;          // longest value name
        DWORD    cbMaxValueData = 0;       // longest value data
        DWORD    cbSecurityDescriptor = 0; // size of security descriptor
        PFILETIME  ftLastWriteTime{};      // last write time

        DWORD retCode;

        // Get the class name and the value count.
        retCode = RegQueryInfoKeyA(
                queryKey,                    // key handle
                achClass,                // buffer for class name
                &cchClassName,           // size of class string
                NULL,                    // reserved
                &cSubKeys,               // number of subkeys
                &cbMaxSubKey,            // longest subkey size
                &cchMaxClass,            // longest class string
                &cValues,                // number of values for this key
                &cchMaxValue,            // longest value name
                &cbMaxValueData,         // longest value data
                &cbSecurityDescriptor,   // security descriptor
                NULL);       // last write time
        TCHAR    achKey[MAX_KEY_LENGTH];   // buffer for subkey name

        std::vector<std::string> matchingDescriptions;


        if (cSubKeys)
        {
            for (int i = 0; i < cSubKeys; i++)
            {
                cbName = MAX_KEY_LENGTH;
                retCode = RegEnumKeyEx(queryKey, i,
                                       achKey,
                                       &cbName,
                                       NULL,
                                       NULL,
                                       NULL,
                                       NULL);
                if (retCode == ERROR_SUCCESS)
                {
                    // For each subkey
                    HKEY subHKey{};

                    DWORD dwType = 0;
                    DWORD size = 256;
                    std::vector<void*> data(256);

                    DWORD ret = RegGetValueA(queryKey, achKey, "DriverDesc", RRF_RT_ANY, &dwType, data.data(), &size);
                    if (ret != 0) {
                        std::cout << "Failed to get DriverDesc\n";
                    }
                    else {
                        std::string description((const char*)data.data());
                        if (description == driverDesc)
                            matchingDescriptions.emplace_back(achKey);

                    }
                }
            }
        }

        std::vector<HKEY> keys;
        for (const auto& keyMatch: matchingDescriptions) {
                HKEY hKey;

                // Open new Key here
                std::string adapterlpKey = "SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e972-e325-11ce-bfc1-08002be10318}\\" + std::string(keyMatch);
                DWORD lResult = RegOpenKeyEx(HKEY_LOCAL_MACHINE, adapterlpKey.c_str(), 0, KEY_READ | KEY_SET_VALUE, &hKey);
                if (lResult != ERROR_SUCCESS) {
                    printf("Failed to retrieve the adapter key\n");
                } else {
                    keys.emplace_back(hKey);
                }
        }
        RegCloseKey(queryKey);
        return keys;
    }


};