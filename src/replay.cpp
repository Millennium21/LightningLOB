// replay.cpp - CSV parsing for OrderReplay::load_csv.
#include "lightninglob/replay.hpp"

#include <cctype>
#include <charconv>
#include <fstream>
#include <stdexcept>

namespace lightninglob {

namespace {

std::vector<std::string_view> split(std::string_view line, char delim) {
    std::vector<std::string_view> fields;
    fields.reserve(8);
    std::size_t start = 0;
    while (start <= line.size()) {
        const std::size_t pos = line.find(delim, start);
        if (pos == std::string_view::npos) {
            fields.push_back(line.substr(start));
            break;
        }
        fields.push_back(line.substr(start, pos - start));
        start = pos + 1;
    }
    return fields;
}

template <typename T>
T parse_number(std::string_view field) {
    T value{};
    const auto [ptr, ec] = std::from_chars(field.data(), field.data() + field.size(), value);
    if (ec != std::errc{}) {
        throw std::runtime_error("OrderReplay: failed to parse numeric field: '" + std::string(field) + "'");
    }
    return value;
}

Side parse_side(std::string_view field) {
    if (field == "BUY") return Side::Buy;
    if (field == "SELL") return Side::Sell;
    throw std::runtime_error("OrderReplay: invalid side field: '" + std::string(field) + "'");
}

OrderType parse_type(std::string_view field) {
    if (field == "LIMIT") return OrderType::Limit;
    if (field == "MARKET") return OrderType::Market;
    throw std::runtime_error("OrderReplay: invalid type field: '" + std::string(field) + "'");
}

TimeInForce parse_tif(std::string_view field) {
    if (field == "GTC" || field.empty()) return TimeInForce::GTC;
    if (field == "IOC") return TimeInForce::IOC;
    throw std::runtime_error("OrderReplay: invalid time_in_force field: '" + std::string(field) + "'");
}

}  // namespace

std::vector<OrderRequest> OrderReplay::load_csv(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("OrderReplay: could not open file: " + path);
    }

    std::vector<OrderRequest> orders;
    std::string line;
    bool first_line = true;

    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();  // tolerate CRLF line endings
        if (line.empty()) continue;

        if (first_line) {
            first_line = false;
            if (!std::isdigit(static_cast<unsigned char>(line[0]))) {
                continue;  // header row (first field isn't numeric) - skip it
            }
        }

        const auto fields = split(line, ',');
        if (fields.size() < 7) {
            throw std::runtime_error("OrderReplay: malformed CSV line (need >= 7 fields): " + line);
        }

        OrderRequest request{};
        request.client_order_id = parse_number<OrderId>(fields[0]);
        parse_number<Timestamp>(fields[1]);  // validated, not otherwise used - see replay.hpp
        request.symbol = parse_number<SymbolId>(fields[2]);
        request.side = parse_side(fields[3]);
        request.type = parse_type(fields[4]);
        request.price = parse_number<Price>(fields[5]);
        request.quantity = parse_number<Quantity>(fields[6]);
        request.time_in_force = (fields.size() > 7) ? parse_tif(fields[7]) : TimeInForce::GTC;

        orders.push_back(request);
    }

    return orders;
}

}  // namespace lightninglob
