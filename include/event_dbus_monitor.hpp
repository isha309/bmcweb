#include <dbus_utility.hpp>
#include <error_messages.hpp>
#include <event_service_manager.hpp>
#include <resource_messages.hpp>

namespace crow
{
namespace dbus_monitor
{

static std::shared_ptr<sdbusplus::bus::match::match> matchHostStateChange;
static std::shared_ptr<sdbusplus::bus::match::match> matchBMCStateChange;
static std::shared_ptr<sdbusplus::bus::match::match>
    matchVMIIPEnabledPropChange;
static std::shared_ptr<sdbusplus::bus::match::match> matchVMIIPChange;
static std::shared_ptr<sdbusplus::bus::match::match> matchDumpCreatedSignal;
static std::shared_ptr<sdbusplus::bus::match::match> matchDumpDeletedSignal;
static std::shared_ptr<sdbusplus::bus::match::match> matchBIOSAttrUpdate;
static std::shared_ptr<sdbusplus::bus::match::match> matchBootProgressChange;
static std::shared_ptr<sdbusplus::bus::match::match> matchEventLogCreated;
static std::shared_ptr<sdbusplus::bus::match::match> matchPostCodeChange;

static uint64_t postCodeCounter = 0;

bool isIPEnabledOnIntfEth0;
bool isIPEnabledOnIntfEth1;

void registerHostStateChangeSignal();
void registerBMCStateChangeSignal();
void registerVMIIPEnabledPropChangeSignal();
void registerVMIIPChangeSignal();
void registerDumpCreatedSignal();
void registerDumpDeletedSignal();
void registerBIOSAttrUpdateSignal();
void registerBootProgressChangeSignal();
void registerEventLogCreatedSignal();
void registerPostCodeChangeSignal();

inline void sendEventOnEthIntf(std::string origin)
{
    redfish::EventServiceManager::getInstance().sendEvent(
        redfish::messages::resourceChanged(), origin, "EthernetInterface");
}

inline void sendEventIfStaticIP(std::string intf)
{
    std::string ethIntfObjPath =
        "/xyz/openbmc_project/network/hypervisor/" + intf;
    crow::connections::systemBus->async_method_call(
        [intf](const boost::system::error_code ec,
               const std::variant<std::string>& isDhcpEnabled) {
            if (ec)
            {
                BMCWEB_LOG_ERROR << "Get DHCPEnabled: DBUS response error "
                                 << ec;
                return;
            }

            const std::string* method =
                std::get_if<std::string>(&isDhcpEnabled);
            if (*method ==
                "xyz.openbmc_project.Network.EthernetInterface.DHCPConf.none")
            {
                // send event only if it is static
                // as the enabled property will be enabled
                // only after the ip configuration is applied
                // on the hypervisor networkd dbus object.
                // In case of dhcp configuration, the enabled
                // property will be set to true before the host
                // sends down the dhcp ip details, in that case
                // events will be sent for each property change
                // on the dbus object.
                std::string origin =
                    "/redfish/v1/Systems/hypervisor/EthernetInterfaces/" + intf;
                if (intf == "eth0" && isIPEnabledOnIntfEth0)
                {
                    BMCWEB_LOG_DEBUG
                        << "Pushing the VMI IP property change event for "
                           "static IP configuration on eth0";
                    sendEventOnEthIntf(origin);
                    return;
                }
                else if (intf == "eth1" && isIPEnabledOnIntfEth1)
                {
                    BMCWEB_LOG_DEBUG
                        << "Pushing the VMI IP property change event for "
                           "static IP configuration on eth1";
                    sendEventOnEthIntf(origin);
                    return;
                }
            }
        },
        "xyz.openbmc_project.Network.Hypervisor", ethIntfObjPath,
        "org.freedesktop.DBus.Properties", "Get",
        "xyz.openbmc_project.Network.EthernetInterface", "DHCPEnabled");
}

inline void setVMIIPEnabledValue(std::string intf)
{
    std::string objPath =
        "/xyz/openbmc_project/network/hypervisor/" + intf + "/ipv4/addr0";
    crow::connections::systemBus->async_method_call(
        [intf](const boost::system::error_code ec,
               const std::variant<bool>& enabled) {
            if (ec)
            {
                BMCWEB_LOG_ERROR << "DBUS response error " << ec;
                return;
            }

            const bool* isEnabled = std::get_if<bool>(&enabled);
            if (intf == "eth0")
            {
                isIPEnabledOnIntfEth0 = *isEnabled;
            }
            else if (intf == "eth1")
            {
                isIPEnabledOnIntfEth1 = *isEnabled;
            }
        },
        "xyz.openbmc_project.Network.Hypervisor", objPath,
        "org.freedesktop.DBus.Properties", "Get",
        "xyz.openbmc_project.Object.Enable", "Enabled");
}

inline void setVMIIPEnabledValue()
{
    setVMIIPEnabledValue("eth0");
    setVMIIPEnabledValue("eth1");
}

inline void VMIIPEnabledPropChange(sdbusplus::message::message& msg)
{
    if (msg.is_method_error())
    {
        BMCWEB_LOG_ERROR
            << "BMC Hypervisor IP Enabled property changed Signal error";
        return;
    }

    std::string objPath = msg.get_path();

    std::string infName;
    dbus::utility::getNthStringFromPath(msg.get_path(), 4, infName);

    if ((objPath ==
         "/xyz/openbmc_project/network/hypervisor/eth0/ipv4/addr0") ||
        (objPath == "/xyz/openbmc_project/network/hypervisor/eth1/ipv4/addr0"))
    {
        boost::container::flat_map<std::string,
                                   std::variant<std::string, uint8_t, bool>>
            values;
        std::string objName;
        msg.read(objName, values);

        auto find = values.find("Enabled");
        if (find == values.end())
        {
            BMCWEB_LOG_ERROR << "Enabled property not Found";
            return;
        }

        const bool* propValue = std::get_if<bool>(&(find->second));
        std::string intf;

        if (objPath.find("/eth0") != std::string::npos)
        {
            isIPEnabledOnIntfEth0 = *propValue;
            intf = "eth0";
        }
        else if (objPath.find("/eth1") != std::string::npos)
        {
            isIPEnabledOnIntfEth1 = *propValue;
            intf = "eth1";
        }
        sendEventIfStaticIP(intf);
    }
}

inline void VMIIPPropertyChange(sdbusplus::message::message& msg)
{
    if (msg.is_method_error())
    {
        BMCWEB_LOG_ERROR << "BMC Hypervisor IP properties changed Signal error";
        return;
    }

    std::string objPath = msg.get_path();

    if ((objPath !=
         "/xyz/openbmc_project/network/hypervisor/eth0/ipv4/addr0") &&
        (objPath != "/xyz/openbmc_project/network/hypervisor/eth1/ipv4/addr0"))
    {
        return;
    }

    std::string infName;
    if (objPath.find("/eth0") != std::string::npos)
    {
        infName = "eth0";
    }
    else if (objPath.find("/eth1") != std::string::npos)
    {
        infName = "eth1";
    }

    boost::container::flat_map<std::string, std::variant<std::string, uint8_t>>
        values;
    std::string objName;
    msg.read(objName, values);

    bool enabledValue;
    if (infName == "eth0")
    {
        enabledValue = isIPEnabledOnIntfEth0;
    }
    else if (infName == "eth1")
    {
        enabledValue = isIPEnabledOnIntfEth1;
    }

    std::string origin =
        "/redfish/v1/Systems/hypervisor/EthernetInterfaces/" + infName;
    auto find = values.find("Address");
    if (find != values.end())
    {
        const std::string propValue = std::get<std::string>(find->second);
        if (enabledValue)
        {
            BMCWEB_LOG_DEBUG
                << "Pushing the VMI IP property change event for Address: "
                << propValue << " with origin : " << origin;
            sendEventOnEthIntf(origin);
            return;
        }
    }

    find = values.find("Gateway");
    if (find != values.end())
    {
        const std::string propValue = std::get<std::string>(find->second);
        if (enabledValue)
        {
            BMCWEB_LOG_DEBUG
                << "Pushing the VMI IP property change event for Gateway: "
                << propValue << " with origin : " << origin;
            sendEventOnEthIntf(origin);
            return;
        }
    }

    find = values.find("PrefixLength");
    if (find != values.end())
    {
        const int64_t propValue =
            static_cast<int64_t>(std::get<uint8_t>(find->second));
        if (enabledValue)
        {
            BMCWEB_LOG_DEBUG
                << "Pushing the VMI IP property change event for PrefixLength: "
                << propValue << " with origin : " << origin;
            sendEventOnEthIntf(origin);
            return;
        }
    }

    find = values.find("Origin");
    if (find != values.end())
    {
        const std::string propValue = std::get<std::string>(find->second);
        if (enabledValue)
        {
            BMCWEB_LOG_DEBUG
                << "Pushing the VMI IP property change event for IP Origin "
                << propValue << " with origin : " << origin;
            sendEventOnEthIntf(origin);
            return;
        }
    }

    find = values.find("Type");
    if (find != values.end())
    {
        // Do nothing
        return;
    }
}

inline void BMCStatePropertyChange(sdbusplus::message::message& msg)
{
    BMCWEB_LOG_DEBUG << "BMC state change match fired";

    if (msg.is_method_error())
    {
        BMCWEB_LOG_ERROR << "BMC state property changed Signal error";
        return;
    }
    std::string iface;
    boost::container::flat_map<std::string, std::variant<std::string, uint8_t>>
        values;
    std::string objName;
    msg.read(objName, values);
    auto find = values.find("CurrentBMCState");
    if (find == values.end())
    {
        return;
    }
    std::string* type = std::get_if<std::string>(&(find->second));
    if (type != nullptr)
    {
        BMCWEB_LOG_DEBUG << *type;
        // Push an event
        std::string origin = "/redfish/v1/Managers/bmc";
        redfish::EventServiceManager::getInstance().sendEvent(
            redfish::messages::resourceChanged(), origin, "Manager");
    }
}

inline void HostStatePropertyChange(sdbusplus::message::message& msg)
{
    BMCWEB_LOG_DEBUG << "Host state change match fired";

    if (msg.is_method_error())
    {
        BMCWEB_LOG_ERROR << "Host state property changed Signal error";
        return;
    }
    std::string iface;
    boost::container::flat_map<std::string, std::variant<std::string, uint8_t>>
        values;
    std::string objName;
    msg.read(objName, values);
    auto find = values.find("CurrentHostState");
    if (find == values.end())
    {
        return;
    }
    std::string* type = std::get_if<std::string>(&(find->second));
    if (type != nullptr)
    {
        BMCWEB_LOG_DEBUG << *type;
        if (*type == "xyz.openbmc_project.State.Host.HostState.Off")
        {
            // reset the postCodeCounter
            postCodeCounter = 0;
            BMCWEB_LOG_DEBUG
                << "Host is powered off. Reset the postcode counter to "
                << postCodeCounter;
        }
        // Push an event
        std::string origin = "/redfish/v1/Systems/system";
        redfish::EventServiceManager::getInstance().sendEvent(
            redfish::messages::resourceChanged(), origin, "ComputerSystem");
    }
}

inline void BootProgressPropertyChange(sdbusplus::message::message& msg)
{
    BMCWEB_LOG_DEBUG << "BootProgress change match fired";

    if (msg.is_method_error())
    {
        BMCWEB_LOG_ERROR << "BootProgress property changed Signal error";
        return;
    }
    std::string iface;
    boost::container::flat_map<std::string, std::variant<std::string, uint8_t>>
        values;
    std::string objName;
    msg.read(objName, values);
    auto find = values.find("BootProgress");
    if (find == values.end())
    {
        return;
    }
    std::string* type = std::get_if<std::string>(&(find->second));
    if (type != nullptr)
    {
        BMCWEB_LOG_DEBUG << *type;
        // Push an event
        std::string origin = "/redfish/v1/Systems/system";
        redfish::EventServiceManager::getInstance().sendEvent(
            redfish::messages::resourceChanged(), origin, "ComputerSystem");
    }
}

inline void postCodePropertyChange(sdbusplus::message::message& msg)
{

    if (msg.is_method_error())
    {
        BMCWEB_LOG_ERROR << "PostCode property changed Signal error";
        return;
    }
    std::string postcodeEntryID = "B1-" + std::to_string(++postCodeCounter);

    BMCWEB_LOG_DEBUG << "Current post code: " << postcodeEntryID;
    // Push an event
    std::string eventOrigin = "/redfish/v1/Systems/system/"
                              "LogServices/PostCodes/Entries/" +
                              postcodeEntryID;
    redfish::EventServiceManager::getInstance().sendEvent(
        redfish::messages::resourceCreated(), eventOrigin, "ComputerSystem");
}

void registerHostStateChangeSignal()
{
    BMCWEB_LOG_DEBUG << "Host state change signal - Register";

    matchHostStateChange = std::make_unique<sdbusplus::bus::match::match>(
        *crow::connections::systemBus,
        "type='signal',member='PropertiesChanged',interface='org.freedesktop."
        "DBus.Properties',path='/xyz/openbmc_project/state/host0',"
        "arg0='xyz.openbmc_project.State.Host'",
        HostStatePropertyChange);
}

void registerBMCStateChangeSignal()
{

    BMCWEB_LOG_DEBUG << "BMC state change signal - Register";

    matchBMCStateChange = std::make_unique<sdbusplus::bus::match::match>(
        *crow::connections::systemBus,
        "type='signal',member='PropertiesChanged',interface='org.freedesktop."
        "DBus.Properties',path='/xyz/openbmc_project/state/bmc0',"
        "arg0='xyz.openbmc_project.State.BMC'",
        BMCStatePropertyChange);
}

void registerVMIIPEnabledPropChangeSignal()
{

    BMCWEB_LOG_DEBUG << "VMI IP change signal match - Registered";

    matchVMIIPEnabledPropChange = std::make_unique<
        sdbusplus::bus::match::match>(
        *crow::connections::systemBus,
        "type='signal',member='PropertiesChanged',interface='org.freedesktop."
        "DBus.Properties',arg0namespace='xyz.openbmc_project.Object.Enable'",
        VMIIPEnabledPropChange);
}

void registerVMIIPChangeSignal()
{
    matchVMIIPChange = std::make_unique<sdbusplus::bus::match::match>(
        *crow::connections::systemBus,
        "type='signal',member='PropertiesChanged',interface='org.freedesktop."
        "DBus.Properties',arg0namespace='xyz.openbmc_project.Network.IP'",
        VMIIPPropertyChange);
}

void registerBootProgressChangeSignal()
{
    BMCWEB_LOG_DEBUG << "BootProgress change signal - Register";

    matchBootProgressChange = std::make_unique<sdbusplus::bus::match::match>(
        *crow::connections::systemBus,
        "type='signal',member='PropertiesChanged',interface='org.freedesktop."
        "DBus.Properties',path='/xyz/openbmc_project/state/host0',"
        "arg0='xyz.openbmc_project.State.Boot.Progress'",
        BootProgressPropertyChange);
}

void eventLogCreatedSignal(sdbusplus::message::message& msg)
{
    BMCWEB_LOG_DEBUG << "Event Log Created - match fired";

    constexpr auto pelEntryInterface = "org.open_power.Logging.PEL.Entry";

    if (msg.is_method_error())
    {
        BMCWEB_LOG_ERROR << "Event Log Created signal error";
        return;
    }

    sdbusplus::message::object_path objPath;
    std::map<std::string,
             std::map<std::string, std::variant<std::string, bool>>>
        interfaces;

    msg.read(objPath, interfaces);

    std::string logID;
    dbus::utility::getNthStringFromPath(objPath, 4, logID);

    const auto pelProperties = interfaces.find(pelEntryInterface);
    if (pelProperties == interfaces.end())
    {
        return;
    }

    const auto hiddenProperty = pelProperties->second.find("Hidden");
    if (hiddenProperty == pelProperties->second.end())
    {
        return;
    }

    const bool* hiddenPropertyPtr =
        std::get_if<bool>(&(hiddenProperty->second));
    if (hiddenPropertyPtr == nullptr)
    {
        BMCWEB_LOG_ERROR << "Failed to get Hidden property";
        return;
    }

    std::string eventOrigin;
    if (*hiddenPropertyPtr)
    {
        eventOrigin =
            "/redfish/v1/Systems/system/LogServices/CELog/Entries/" + logID;
        BMCWEB_LOG_DEBUG << "CELog path: " << eventOrigin;
    }
    else
    {
        eventOrigin =
            "/redfish/v1/Systems/system/LogServices/EventLog/Entries/" + logID;
        BMCWEB_LOG_DEBUG << "EventLog path: " << eventOrigin;
    }

    BMCWEB_LOG_DEBUG << "Sending event for log ID " << logID;
    redfish::EventServiceManager::getInstance().sendEvent(
        redfish::messages::resourceCreated(), eventOrigin, "LogEntry");
}

void registerEventLogCreatedSignal()
{
    BMCWEB_LOG_DEBUG << "Register EventLog Created Signal";
    matchEventLogCreated = std::make_unique<sdbusplus::bus::match::match>(
        *crow::connections::systemBus,
        "type='signal',member='InterfacesAdded',interface='org.freedesktop."
        "DBus.ObjectManager',path='/xyz/openbmc_project/logging',",
        eventLogCreatedSignal);
}

void registerStateChangeSignal()
{
    registerHostStateChangeSignal();
    registerBMCStateChangeSignal();

    // Get vmi ip enabled property and set it to the local variable
    // setVMIIPEnabledValue();

    registerVMIIPEnabledPropChangeSignal();
    registerVMIIPChangeSignal();
    registerBootProgressChangeSignal();
}

void registerPostCodeChangeSignal()
{
    BMCWEB_LOG_DEBUG << "PostCode change signal - Register";

    matchPostCodeChange = std::make_unique<sdbusplus::bus::match::match>(
        *crow::connections::systemBus,
        "type='signal',member='PropertiesChanged',interface='org.freedesktop."
        "DBus.Properties',path='/xyz/openbmc_project/state/boot/raw0',"
        "arg0='xyz.openbmc_project.State.Boot.Raw'",
        postCodePropertyChange);
}

inline void dumpCreatedSignal(sdbusplus::message::message& msg)
{
    BMCWEB_LOG_DEBUG << "Dump Created - match fired";

    if (msg.is_method_error())
    {
        BMCWEB_LOG_ERROR << "Dump Created signal error";
        return;
    }

    std::string dumpType;
    std::string dumpId;

    dbus::utility::getNthStringFromPath(msg.get_path(), 3, dumpType);
    dbus::utility::getNthStringFromPath(msg.get_path(), 5, dumpId);

    boost::container::flat_map<std::string, std::variant<std::string, uint8_t>>
        values;
    std::string objName;
    msg.read(objName, values);

    auto find = values.find("Status");
    if (find == values.end())
    {
        BMCWEB_LOG_DEBUG
            << "Status property not found. Continuing to listen...";
        return;
    }
    std::string* propValue = std::get_if<std::string>(&(find->second));

    if (propValue != nullptr &&
        *propValue ==
            "xyz.openbmc_project.Common.Progress.OperationStatus.Completed")
    {
        BMCWEB_LOG_DEBUG << "Sending event\n";

        std::string eventOrigin;
        // Push an event
        if (dumpType == "bmc")
        {
            eventOrigin =
                "/redfish/v1/Managers/bmc/LogServices/Dump/Entries/" + dumpId;
        }
        else if (dumpType == "system")
        {
            eventOrigin =
                "/redfish/v1/Systems/system/LogServices/Dump/Entries/System_" +
                dumpId;
        }
        else if (dumpType == "resource")
        {
            eventOrigin = "/redfish/v1/Systems/system/LogServices/Dump/Entries/"
                          "Resource_" +
                          dumpId;
        }
        else if (dumpType == "hostboot")
        {
            eventOrigin = "/redfish/v1/Systems/system/LogServices/Dump/Entries/"
                          "Hostboot_" +
                          dumpId;
        }
        else if (dumpType == "hardware")
        {
            eventOrigin = "/redfish/v1/Systems/system/LogServices/Dump/Entries/"
                          "Hardware_" +
                          dumpId;
        }
        else if (dumpType == "sbe")
        {
            eventOrigin = "/redfish/v1/Systems/system/LogServices/Dump/Entries/"
                          "SBE_" +
                          dumpId;
        }
        else
        {
            BMCWEB_LOG_ERROR << "Invalid dump type received when listening for "
                                "dump created signal";
            return;
        }
        redfish::EventServiceManager::getInstance().sendEvent(
            redfish::messages::resourceCreated(), eventOrigin, "LogEntry");
    }
}

inline void dumpDeletedSignal(sdbusplus::message::message& msg)
{
    BMCWEB_LOG_DEBUG << "Dump Deleted - match fired";

    if (msg.is_method_error())
    {
        BMCWEB_LOG_ERROR << "Dump Deleted signal error";
        return;
    }

    std::vector<std::string> interfacesList;
    sdbusplus::message::object_path objPath;

    msg.read(objPath, interfacesList);

    std::string dumpType;
    std::string dumpId;

    dbus::utility::getNthStringFromPath(objPath, 3, dumpType);
    dbus::utility::getNthStringFromPath(objPath, 5, dumpId);

    std::string eventOrigin;

    if (dumpType == "bmc")
    {
        eventOrigin =
            "/redfish/v1/Managers/bmc/LogServices/Dump/Entries/" + dumpId;
    }
    else if (dumpType == "system")
    {
        eventOrigin =
            "/redfish/v1/Systems/system/LogServices/Dump/Entries/System_" +
            dumpId;
    }
    else if (dumpType == "resource")
    {
        eventOrigin =
            "/redfish/v1/Systems/system/LogServices/Dump/Entries/Resource_" +
            dumpId;
    }
    else if (dumpType == "hostboot")
    {
        eventOrigin =
            "/redfish/v1/Systems/system/LogServices/Dump/Entries/Hostboot_" +
            dumpId;
    }
    else if (dumpType == "hardware")
    {
        eventOrigin =
            "/redfish/v1/Systems/system/LogServices/Dump/Entries/Hardware_" +
            dumpId;
    }
    else if (dumpType == "sbe")
    {
        eventOrigin =
            "/redfish/v1/Systems/system/LogServices/Dump/Entries/SBE_" + dumpId;
    }
    else
    {
        BMCWEB_LOG_ERROR << "Invalid dump type received when listening for "
                            "dump deleted signal";
        return;
    }

    redfish::EventServiceManager::getInstance().sendEvent(
        redfish::messages::resourceRemoved(), eventOrigin, "LogEntry");
}

void registerDumpCreatedSignal()
{
    BMCWEB_LOG_DEBUG << "Dump Created signal - Register";

    matchDumpCreatedSignal = std::make_unique<sdbusplus::bus::match::match>(
        *crow::connections::systemBus,
        "type='signal',member='PropertiesChanged',interface='org.freedesktop."
        "DBus.Properties',arg0namespace='xyz.openbmc_project.Common.Progress',",
        dumpCreatedSignal);
}

void registerDumpDeletedSignal()
{
    BMCWEB_LOG_DEBUG << "Dump Deleted signal - Register";

    matchDumpDeletedSignal = std::make_unique<sdbusplus::bus::match::match>(
        *crow::connections::systemBus,
        "type='signal',member='InterfacesRemoved',interface='org.freedesktop."
        "DBus.ObjectManager',path='/xyz/openbmc_project/dump',",
        dumpDeletedSignal);
}

void registerDumpUpdateSignal()
{
    registerDumpCreatedSignal();
    registerDumpDeletedSignal();
}

inline void BIOSAttrUpdate(sdbusplus::message::message& msg)
{
    BMCWEB_LOG_DEBUG << "BIOS attribute change match fired";

    if (msg.is_method_error())
    {
        BMCWEB_LOG_ERROR << "BIOS attribute changed Signal error";
        return;
    }

    boost::container::flat_map<std::string, std::variant<std::string, uint8_t>>
        values;
    std::string objName;
    msg.read(objName, values);

    auto find = values.find("BaseBIOSTable");
    if (find == values.end())
    {
        BMCWEB_LOG_DEBUG
            << "BaseBIOSTable property not found. Continuing to listen...";
        return;
    }
    std::string* type = std::get_if<std::string>(&(find->second));
    if (type != nullptr)
    {
        BMCWEB_LOG_DEBUG << "Sending event\n";
        // Push an event
        std::string origin = "/redfish/v1/Systems/system/Bios";
        redfish::EventServiceManager::getInstance().sendEvent(
            redfish::messages::resourceChanged(), origin, "Bios");
    }
}

void registerBIOSAttrUpdateSignal()
{
    BMCWEB_LOG_DEBUG << "BIOS Attribute update signal match - Registered";

    matchBIOSAttrUpdate = std::make_unique<sdbusplus::bus::match::match>(
        *crow::connections::systemBus,
        "type='signal',member='PropertiesChanged',interface='org.freedesktop."
        "DBus.Properties',arg0namespace='xyz.openbmc_project.BIOSConfig."
        "Manager'",
        BIOSAttrUpdate);
}

} // namespace dbus_monitor
} // namespace crow
