export module tls;
import std;


export namespace tls {
struct address {
    std::string host, port, authority, target;
    static auto parse(std::string_view url) -> address {
        if (!url.starts_with("wss://"))
            throw std::invalid_argument("Worker gateway must use wss://");
        url.remove_prefix(6);
        auto slash = url.find('/');
        if (slash == std::string_view::npos)
            throw std::invalid_argument("Worker gateway needs a tunnel path");
        address out;
        out.authority = url.substr(0, slash);
        out.target = url.substr(slash);
        if (out.authority.empty() || out.authority.find_first_of("@[]?# \\\r\n\t") != std::string::npos ||
            out.target.find_first_of("?# \\\r\n\t") != std::string::npos)
            throw std::invalid_argument("Worker gateway URL is invalid");
        auto colon = out.authority.find(':');
        out.host = out.authority.substr(0, colon);
        out.port = colon == std::string::npos ? "443" : out.authority.substr(colon + 1);
        unsigned port = 0;
        auto [end, ec] = std::from_chars(out.port.data(), out.port.data() + out.port.size(), port);
        if (out.host.empty() || ec != std::errc{} || end != out.port.data() + out.port.size() || !port || port > 65535)
            throw std::invalid_argument("Worker gateway host or port is invalid");
        if (std::ranges::all_of(out.host, [](char c) { return (c >= '0' && c <= '9') || c == '.'; }))
            throw std::invalid_argument("Worker WSS gateway requires a DNS hostname");
        return out;
    }
};

} // namespace tls
