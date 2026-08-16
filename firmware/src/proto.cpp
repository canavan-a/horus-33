#include "proto.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace proto {
namespace {

SemaphoreHandle_t txMutex = nullptr;

// Two hex digits to a byte. Returns -1 on a non-hex digit.
int hexPair(const char *s) {
	int hi = -1, lo = -1;
	auto nib = [](char c) -> int {
		if (c >= '0' && c <= '9') return c - '0';
		if (c >= 'a' && c <= 'f') return c - 'a' + 10;
		if (c >= 'A' && c <= 'F') return c - 'A' + 10;
		return -1;
	};
	hi = nib(s[0]);
	lo = nib(s[1]);
	if (hi < 0 || lo < 0) return -1;
	return hi * 16 + lo;
}

} // namespace

void begin(unsigned long baud) {
	txMutex = xSemaphoreCreateMutex();
	Serial.begin(baud);
}

void send(const JsonDocument &doc) {
	// If begin() has not run yet, fall through unguarded rather than dropping
	// the line -- this only happens during early boot when we are single-tasked.
	if (txMutex != nullptr) {
		xSemaphoreTake(txMutex, portMAX_DELAY);
	}
	serializeJson(doc, Serial);
	Serial.write('\n');
	if (txMutex != nullptr) {
		xSemaphoreGive(txMutex);
	}
}

void sendHello(const char *device, const char *fw) {
	JsonDocument doc;
	doc["t"] = T_HELLO;
	doc["proto"] = VERSION;
	doc["device"] = device;
	doc["fw"] = fw;
	send(doc);
}

void sendAck(uint32_t seq) {
	JsonDocument doc;
	doc["t"] = T_ACK;
	doc["seq"] = seq;
	send(doc);
}

void sendErr(uint32_t seq, const char *msg) {
	JsonDocument doc;
	doc["t"] = T_ERR;
	doc["seq"] = seq;
	doc["msg"] = msg;
	send(doc);
}

void formatColor(uint8_t r, uint8_t g, uint8_t b, char *out) {
	snprintf(out, 8, "#%02x%02x%02x", r, g, b);
}

bool parseColor(const char *s, uint8_t &r, uint8_t &g, uint8_t &b) {
	if (s == nullptr) return false;
	if (*s == '#') s++;
	if (strlen(s) != 6) return false;

	int cr = hexPair(s);
	int cg = hexPair(s + 2);
	int cb = hexPair(s + 4);
	if (cr < 0 || cg < 0 || cb < 0) return false;

	r = (uint8_t)cr;
	g = (uint8_t)cg;
	b = (uint8_t)cb;
	return true;
}

} // namespace proto
