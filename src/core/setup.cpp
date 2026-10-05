#include "core/setup.h"

#include <algorithm>
#include <memory>
#include <format>

#include "core/log.h"
#include "db/sym/sym_parser.h"

namespace
{

bool attr_bool(const pugi::xml_node& el, const char* name, bool def)
{
    return el.attribute(name).as_int(def ? 1 : 0) != 0;
}

void set_bool(pugi::xml_node& el, const char* name, bool v)
{
    el.append_attribute(name) = v ? 1 : 0;
}

int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Like QByteArray::fromHex: non-hex characters are skipped.
std::vector<uint8_t> from_hex(std::string_view s)
{
    std::vector<uint8_t> out;
    int hi = -1;
    for (const char c : s)
    {
        const int v = hex_value(c);
        if (v < 0) continue;
        if (hi < 0)
        {
            hi = v;
        }
        else
        {
            out.push_back(static_cast<uint8_t>(hi << 4 | v));
            hi = -1;
        }
    }
    return out;
}

void network_load_xml(SetupNetwork& net, const pugi::xml_node& el)
{
    net.name = el.attribute("name").as_string("unnamed network");

    for (const pugi::xml_node i : el.child("interfaces").children("interface"))
    {
        SetupInterface& intf = net.interfaces.emplace_back();
        setup_interface_load_xml(intf, i);
    }

    for (const pugi::xml_node d : el.child("databases").children("database"))
    {
        const std::string filename = d.attribute("filename").as_string();
        if (filename.empty())
        {
            log_error("Unable to load database: empty filename");
            continue;
        }
        if (std::string_view(d.attribute("db-type").as_string("dbc")) == "ldf")
        {
            auto db = std::make_shared<LinDb>();
            if (lin_db_load(*db, filename)) net.lin_dbs.push_back(std::move(db));
            else log_error(std::format("Unable to load LDF: {} ({})", filename, db->last_error));
        }
        else
        {
            auto db = std::make_shared<CanDb>();
            if (can_db_parse_file(filename, *db)) net.can_dbs.push_back(std::move(db));
            else log_error(std::format("Unable to load DBC: {}", filename));
        }
    }
}

void network_save_xml(const SetupNetwork& net, pugi::xml_node& el)
{
    el.append_attribute("name") = net.name.c_str();

    pugi::xml_node interfaces = el.append_child("interfaces");
    for (const SetupInterface& intf : net.interfaces)
    {
        pugi::xml_node i = interfaces.append_child("interface");
        setup_interface_save_xml(intf, i);
    }

    pugi::xml_node databases = el.append_child("databases");
    for (const auto& db : net.can_dbs)
    {
        pugi::xml_node d = databases.append_child("database");
        d.append_attribute("db-type") = "dbc";
        d.append_attribute("type") = "dbc";
        d.append_attribute("filename") = db->path.c_str();
    }
    for (const auto& db : net.lin_dbs)
    {
        pugi::xml_node d = databases.append_child("database");
        d.append_attribute("db-type") = "ldf";
        d.append_attribute("filename") = db->path.c_str();
    }
}

} // namespace

// Defaults are the attribute defaults of MeasurementInterface::loadXML, which differ from
// the struct defaults in two places (bitrate-fd 500000, lin-node-mode 0 = Monitor).
void setup_interface_load_xml(SetupInterface& intf, const pugi::xml_node& el)
{
    intf.driver = el.attribute("driver").as_string();
    intf.name = el.attribute("name").as_string();
    intf.iface = -1;

    intf.bus_type = std::string_view(el.attribute("bus-type").as_string("can")) == "lin" ? BusType::LIN : BusType::CAN;
    intf.configure = attr_bool(el, "configure", false);

    intf.bitrate = el.attribute("bitrate").as_uint(500000);
    intf.sample_point = el.attribute("sample-point").as_uint(875);
    intf.can_fd = attr_bool(el, "can-fd", false);
    intf.fd_bitrate = el.attribute("bitrate-fd").as_uint(500000);
    intf.fd_sample_point = el.attribute("sample-point-fd").as_uint(875);

    intf.listen_only = attr_bool(el, "listen-only", false);
    intf.one_shot = attr_bool(el, "one-shot", false);
    intf.triple_sampling = attr_bool(el, "triple-sampling", false);
    intf.auto_restart = attr_bool(el, "auto-restart", false);
    intf.auto_restart_ms = el.attribute("auto-restart-time").as_int(100);

    intf.is_custom_bitrate = attr_bool(el, "is-custom-bitrate", false);
    intf.is_custom_fd_bitrate = attr_bool(el, "is-custom-fdbitrate", false);
    intf.custom_bitrate = el.attribute("custom-bitrate").as_uint(0);
    intf.custom_fd_bitrate = el.attribute("custom-fdbitrate").as_uint(0);
    intf.enabled = attr_bool(el, "enabled", true);

    intf.lin_baudrate = el.attribute("lin-baudrate").as_uint(19200);
    intf.lin_protocol = static_cast<LinProtocolVersion>(el.attribute("lin-protocol").as_int(4));
    intf.lin_node_mode = static_cast<LinNodeMode>(el.attribute("lin-node-mode").as_int(0));
    intf.lin_listen_only = attr_bool(el, "lin-listen-only", false);
    intf.lin_checksum_classic = attr_bool(el, "lin-checksum-classic", false);
    intf.lin_ldf_path = el.attribute("lin-ldf-path").as_string();
    intf.lin_schedule_table = el.attribute("lin-schedule-table").as_string();
    intf.lin_slave_node = el.attribute("lin-slave-node").as_string();
    intf.lin_timebase_ms = static_cast<uint8_t>(el.attribute("lin-timebase-ms").as_uint(5));
    intf.lin_jitter_us = static_cast<uint16_t>(el.attribute("lin-jitter-us").as_uint(0));

    intf.lin_frame_defaults.clear();
    for (const pugi::xml_node f : el.child("lin-frame-defaults").children("frame"))
    {
        const auto id = static_cast<uint8_t>(f.attribute("id").as_uint(0));
        intf.lin_frame_defaults[id] = from_hex(f.attribute("data").as_string());
    }
}

void setup_interface_save_xml(const SetupInterface& intf, pugi::xml_node& el)
{
    const char* bus = intf.bus_type == BusType::LIN ? "lin" : "can";
    el.append_attribute("bus-type") = bus;
    el.append_attribute("type") = bus;
    el.append_attribute("driver") = intf.driver.c_str();
    el.append_attribute("name") = intf.name.c_str();

    set_bool(el, "configure", intf.configure);

    el.append_attribute("bitrate") = intf.bitrate;
    el.append_attribute("sample-point") = intf.sample_point;
    set_bool(el, "can-fd", intf.can_fd);
    el.append_attribute("bitrate-fd") = intf.fd_bitrate;
    el.append_attribute("sample-point-fd") = intf.fd_sample_point;

    set_bool(el, "listen-only", intf.listen_only);
    set_bool(el, "one-shot", intf.one_shot);
    set_bool(el, "triple-sampling", intf.triple_sampling);
    set_bool(el, "auto-restart", intf.auto_restart);
    el.append_attribute("auto-restart-time") = intf.auto_restart_ms;

    set_bool(el, "is-custom-bitrate", intf.is_custom_bitrate);
    set_bool(el, "is-custom-fdbitrate", intf.is_custom_fd_bitrate);
    el.append_attribute("custom-bitrate") = intf.custom_bitrate;
    el.append_attribute("custom-fdbitrate") = intf.custom_fd_bitrate;
    set_bool(el, "enabled", intf.enabled);

    el.append_attribute("lin-baudrate") = intf.lin_baudrate;
    el.append_attribute("lin-protocol") = static_cast<int>(intf.lin_protocol);
    el.append_attribute("lin-node-mode") = static_cast<int>(intf.lin_node_mode);
    set_bool(el, "lin-listen-only", intf.lin_listen_only);
    set_bool(el, "lin-checksum-classic", intf.lin_checksum_classic);
    el.append_attribute("lin-ldf-path") = intf.lin_ldf_path.c_str();
    el.append_attribute("lin-schedule-table") = intf.lin_schedule_table.c_str();
    el.append_attribute("lin-slave-node") = intf.lin_slave_node.c_str();
    el.append_attribute("lin-timebase-ms") = static_cast<unsigned>(intf.lin_timebase_ms);
    el.append_attribute("lin-jitter-us") = static_cast<unsigned>(intf.lin_jitter_us);

    if (!intf.lin_frame_defaults.empty())
    {
        pugi::xml_node fds = el.append_child("lin-frame-defaults");
        for (const auto& [id, data] : intf.lin_frame_defaults)
        {
            pugi::xml_node f = fds.append_child("frame");
            f.append_attribute("id") = static_cast<unsigned>(id);
            std::string hex;
            append_hex_bytes(hex, data);
            f.append_attribute("data") = hex.c_str();
        }
    }
}

void setup_load_xml(Setup& setup, const pugi::xml_node& el)
{
    setup.networks.clear();
    for (const pugi::xml_node n : el.children("network"))
    {
        network_load_xml(setup.networks.emplace_back(), n);
    }
    setup_rebuild_cache(setup);
}

void setup_save_xml(const Setup& setup, pugi::xml_node& root)
{
    for (const SetupNetwork& net : setup.networks)
    {
        pugi::xml_node n = root.append_child("network");
        network_save_xml(net, n);
    }
}

bool setup_reload_databases(Setup& setup, std::vector<std::string>* errors)
{
    bool ok = true;
    const auto fail = [&](const std::string& path, std::string_view why)
    {
        ok = false;
        if (errors) errors->push_back(std::format("{}: {}", path, why));
    };

    for (SetupNetwork& net : setup.networks)
    {
        for (const auto& db : net.can_dbs)
        {
            CanDb fresh;
            if (can_db_parse_file(db->path, fresh)) can_db_update_from(*db, fresh);
            else fail(db->path, "DBC parse failed");
        }
        for (const auto& db : net.lin_dbs)
        {
            if (!lin_db_load(*db, db->path)) fail(db->path, db->last_error);
        }
    }
    setup_rebuild_cache(setup);
    return ok;
}

bool setup_add_can_db(SetupNetwork& net, const std::string& path)
{
    CanDb fresh;
    if (!can_db_parse_file(path, fresh))
    {
        return false;
    }
    fresh.path = path;
    const auto dup = std::ranges::find_if(net.can_dbs, [&](const auto& d) { return d->path == path; });
    if (dup != net.can_dbs.end())
    {
        can_db_update_from(**dup, fresh);
    }
    else
    {
        net.can_dbs.push_back(std::make_shared<CanDb>(std::move(fresh)));
    }
    return true;
}

void setup_rebuild_cache(Setup& setup)
{
    for (SetupNetwork& net : setup.networks)
    {
        net.can_messages.clear();
        net.lin_frames.clear();
        for (const auto& db : net.can_dbs)
        {
            for (auto& [raw_id, msg] : db->messages)
            {
                net.can_messages[raw_id] = &msg;
            }
        }
        for (const auto& db : net.lin_dbs)
        {
            for (auto& [id, frame] : db->frames)
            {
                net.lin_frames[id] = &frame;
            }
        }
    }
    ++setup.generation;
}

int setup_network_of(const Setup& setup, uint16_t iface)
{
    for (std::size_t n = 0; n < setup.networks.size(); ++n)
    {
        if (std::ranges::find(setup.networks[n].interfaces, int{iface}, &SetupInterface::iface)
            != setup.networks[n].interfaces.end())
        {
            return static_cast<int>(n);
        }
    }
    return -1;
}

namespace
{

// Looks `key` up in the network of `iface`, or in every network if it has none.
template<typename Map, typename Key>
auto find_in_networks(const Setup& setup, Map SetupNetwork::*map, uint16_t iface, Key key)
    -> typename Map::mapped_type
{
    const int n = setup_network_of(setup, iface);
    for (std::size_t i = n < 0 ? 0 : static_cast<std::size_t>(n); i < setup.networks.size(); ++i)
    {
        const Map& m = setup.networks[i].*map;
        if (const auto it = m.find(key); it != m.end()) return it->second;
        if (n >= 0) break;
    }
    return nullptr;
}

} // namespace

const CanDbMessage* setup_find_can_message(const Setup& setup, const BusMessage& m)
{
    // Error frames carry no CAN id (drivers set it to 0): never match a DBC entry.
    if (m.type != BusType::CAN || m.errors != 0) return nullptr;
    // DBC raw ids keep bit 31 for extended frames.
    const uint32_t key = can_id(m) | (has_flag(m, bus_flag::extended) ? 0x80000000u : 0u);
    return find_in_networks(setup, &SetupNetwork::can_messages, m.iface, key);
}

const LinFrame* setup_find_lin_frame(const Setup& setup, const BusMessage& m)
{
    if (m.type != BusType::LIN) return nullptr;
    return find_in_networks(setup, &SetupNetwork::lin_frames, m.iface, static_cast<uint8_t>(m.id & 0x3F));
}

SetupNetwork* setup_find_network(Setup& setup, std::string_view name)
{
    const auto it = std::ranges::find(setup.networks, name, &SetupNetwork::name);
    return it != setup.networks.end() ? &*it : nullptr;
}
