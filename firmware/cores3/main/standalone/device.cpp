#include "device.hpp"
#include "../runtime.hpp"
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <algorithm>
#include <cstring>
#include <memory>

namespace qb::local {
namespace {
int digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
bool decode(const cJSON* value, uint8_t* output, size_t size) {
    if (!cJSON_IsString(value) || !value->valuestring || std::strlen(value->valuestring) != size * 2) return false;
    for (size_t i = 0; i < size; ++i) {
        const int high = digit(value->valuestring[i * 2]), low = digit(value->valuestring[i * 2 + 1]);
        if (high < 0 || low < 0) return false;
        output[i] = uint8_t(high * 16 + low);
    }
    return true;
}
bool number(const cJSON* root, const char* name, unsigned expected) {
    const auto* value = cJSON_GetObjectItemCaseSensitive(root, name);
    return cJSON_IsNumber(value) && value->valuedouble == expected;
}
Bytes prefix(const HfIdentity& card, unsigned address) {
    if ((card.uid_size != 4 && card.uid_size != 7 && card.uid_size != 10) || address > 65535) return {};
    Bytes data{card.uid_size};
    data.insert(data.end(), card.uid.begin(), card.uid.begin() + card.uid_size);
    data.push_back(uint8_t(address)); data.push_back(uint8_t(address >> 8));
    return data;
}
bool auth(Bytes& data, const NtagAuth& value) {
    if (value.mode > 2) return false;
    data.push_back(value.mode);
    if (value.mode) data.insert(data.end(), value.password.begin(), value.password.end());
    if (value.mode == 2) data.insert(data.end(), value.pack.begin(), value.pack.end());
    return true;
}
}
bool DevicePort::cancelled() const { return stop_operation || !local_operation_allowed(epoch_); }
uint32_t DevicePort::generation(uint8_t channel) const {
    if (channel < 1 || channel > 3 || cancelled()) return 0;
    std::lock_guard<std::mutex> guard(state_mutex);
    const auto& slot = slots[channel - 1];
    return slot.present && slot.generation == generations_[channel - 1] ? slot.generation : 0;
}
bool DevicePort::supports(uint8_t channel, uint16_t command) const {
    return generation(channel) && hardware_command_supported(channel, command);
}
void DevicePort::progress(unsigned done, unsigned total, const char* detail) {
    if (progress_) progress_(done, total, detail);
}
void DevicePort::wait_ms(unsigned milliseconds) {
    for (unsigned elapsed = 0; elapsed < milliseconds && !cancelled(); elapsed += 10)
        vTaskDelay(pdMS_TO_TICKS(std::min(10u, milliseconds - elapsed)));
}
Status DevicePort::call(uint8_t channel, uint8_t operation, const Bytes& payload, Bytes& output) {
    if (cancelled()) return CANCELLED;
    const auto before = generation(channel);
    if (!before) return STALE_GENERATION;
    Frame frame; frame.channel = channel; frame.op = operation; frame.generation = before; frame.payload = payload;
    // Session zero is reserved for the local worker and can never be admitted over BLE.
    frame.session = 0;
    used_ |= uint8_t(1u << (channel - 1));
    const Status status = operate(frame, output);
    if (status != OK) return status;
    if (!generation(channel)) return operation == 18 || operation == 20 || operation == 22 || operation == 23 ? WRITE_UNVERIFIED : STALE_GENERATION;
    return OK;
}
Status DevicePort::identify(HfIdentity& result) {
    Bytes output;
    auto status = call(1, 16, {}, output);
    if (status != OK) return status;
    std::string text(output.begin(), output.end());
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_ParseWithLength(text.c_str(), text.size() + 1), cJSON_Delete);
    if (!root || !cJSON_IsObject(root.get())) return IO_ERROR;
    const auto* family = cJSON_GetObjectItemCaseSensitive(root.get(), "family");
    const auto* uid = cJSON_GetObjectItemCaseSensitive(root.get(), "uid");
    const auto* sak = cJSON_GetObjectItemCaseSensitive(root.get(), "sak");
    if (!cJSON_IsString(family) || !family->valuestring || !cJSON_IsString(uid) || !uid->valuestring ||
        !cJSON_IsNumber(sak) || sak->valuedouble < 0 || sak->valuedouble > 255 ||
        sak->valuedouble != sak->valueint) return IO_ERROR;
    HfIdentity card;
    const size_t uid_size = std::strlen(uid->valuestring) / 2;
    if (uid_size != 4 && uid_size != 7 && uid_size != 10) return IO_ERROR;
    card.uid_size = uint8_t(uid_size); card.sak = uint8_t(sak->valueint);
    if (!decode(uid, card.uid.data(), uid_size) ||
        !decode(cJSON_GetObjectItemCaseSensitive(root.get(), "atqa"), card.atqa.data(), 2)) return IO_ERROR;
    const char* name = family->valuestring;
    if (!std::strcmp(name, "classic1k")) card.family = HfFamily::Classic1K;
    else if (!std::strcmp(name, "classic4k")) card.family = HfFamily::Classic4K;
    else if (!std::strcmp(name, "ntag213")) card.family = HfFamily::Ntag213;
    else if (!std::strcmp(name, "ntag215")) card.family = HfFamily::Ntag215;
    else if (!std::strcmp(name, "ntag216")) card.family = HfFamily::Ntag216;
    else if (std::strcmp(name, "unknown")) return IO_ERROR;
    if (!number(root.get(), "blockCount", card.classic() ? card.units() : 0) ||
        !number(root.get(), "pageCount", card.ntag() ? card.units() : 0)) return IO_ERROR;
    if (card.classic() && classic_geometry(card.sak, card.atqa.data(), card.uid_size) != card.units()) return IO_ERROR;
    if (card.ntag()) {
        const uint8_t storage = card.family == HfFamily::Ntag213 ? 0x0f : card.family == HfFamily::Ntag215 ? 0x11 : 0x13;
        const std::array<uint8_t, 8> expected{{0, 4, 4, 2, 1, 0, storage, 3}};
        if (!decode(cJSON_GetObjectItemCaseSensitive(root.get(), "version"), card.version.data(), 8) ||
            card.version != expected || card.uid_size != 7 || card.sak != 0 || card.atqa != std::array<uint8_t, 2>{{0x44, 0}})
            return IO_ERROR;
    }
    result = card;
    return OK;
}
Status DevicePort::read_classic(const HfIdentity& card, unsigned block, const ClassicKey& key,
                                std::array<uint8_t, 16>& result) {
    Bytes data = prefix(card, block), output;
    if (data.empty() || !card.classic() || block >= card.units() || (key.type != 0x60 && key.type != 0x61))
        return INVALID_ARGUMENT;
    data.push_back(key.type); data.insert(data.end(), key.bytes.begin(), key.bytes.end());
    auto status = call(1, 17, data, output);
    if (status != OK) return status;
    if (output.size() != result.size()) return IO_ERROR;
    std::copy(output.begin(), output.end(), result.begin()); return OK;
}
Status DevicePort::write_classic(const HfIdentity& card, unsigned block, const ClassicKey& key,
                                 const std::array<uint8_t, 16>& previous,
                                 const std::array<uint8_t, 16>& desired, std::array<uint8_t, 16>& verified) {
    Bytes data = prefix(card, block), output;
    if (data.empty() || !card.classic() || !classic_data(card.units(), block) || (key.type != 0x60 && key.type != 0x61))
        return PERMISSION_DENIED;
    data.push_back(key.type); data.insert(data.end(), key.bytes.begin(), key.bytes.end());
    data.insert(data.end(), previous.begin(), previous.end()); data.insert(data.end(), desired.begin(), desired.end());
    auto status = call(1, 18, data, output);
    if (status != OK) return status;
    if (output.size() != desired.size() || !std::equal(output.begin(), output.end(), desired.begin())) return WRITE_UNVERIFIED;
    std::copy(output.begin(), output.end(), verified.begin()); return OK;
}
Status DevicePort::read_pages(const HfIdentity& card, unsigned page, unsigned count, const NtagAuth& credential, Bytes& output) {
    Bytes data = prefix(card, page);
    if (data.empty() || !card.ntag() || !count || count > 16 || page + count > card.units()) return INVALID_ARGUMENT;
    data.push_back(uint8_t(count)); if (!auth(data, credential)) return INVALID_ARGUMENT;
    auto status = call(1, 19, data, output);
    return status == OK && output.size() != count * 4 ? IO_ERROR : status;
}
Status DevicePort::write_page(const HfIdentity& card, unsigned page, const NtagAuth& credential,
                              const std::array<uint8_t, 4>& previous,
                              const std::array<uint8_t, 4>& desired, std::array<uint8_t, 4>& verified) {
    Bytes data = prefix(card, page), output;
    if (data.empty() || !card.ntag() || page < 4 || page > ntag(card.units()).last_user || !auth(data, credential))
        return PERMISSION_DENIED;
    data.insert(data.end(), previous.begin(), previous.end()); data.insert(data.end(), desired.begin(), desired.end());
    auto status = call(1, 20, data, output);
    if (status != OK) return status;
    if (output.size() != desired.size() || !std::equal(output.begin(), output.end(), desired.begin())) return WRITE_UNVERIFIED;
    std::copy(output.begin(), output.end(), verified.begin()); return OK;
}
Status DevicePort::reset_hf() { Bytes output; return call(1, 21, {}, output); }
Status DevicePort::format_ndef(const HfIdentity& card, uint8_t flags, const Bytes& expected) {
    const uint8_t op = card.family == HfFamily::Classic1K ? 22 : card.ntag() ? 23 : 0;
    if (!op || !supports(1,op)) return UNSUPPORTED;
    if (flags > 1 || (card.uid_size != 4 && card.uid_size != 7 && card.uid_size != 10)) return INVALID_ARGUMENT;
    Bytes payload{card.uid_size}, output;
    payload.insert(payload.end(),card.uid.begin(),card.uid.begin()+card.uid_size);
    payload.push_back(flags); payload.insert(payload.end(),expected.begin(),expected.end());
    Frame shape; shape.op = op; shape.payload = payload;
    if (validate_hf(shape) != OK) return INVALID_ARGUMENT;
    auto status = call(1,op,payload,output);
    return status == OK && !output.empty() ? WRITE_UNVERIFIED : status;
}
Status DevicePort::transact(uint8_t channel, const Bytes& command, Bytes& response) {
    if (channel != 2 && channel != 3) return INVALID_ARGUMENT;
    return call(channel, 32, command, response);
}
}
