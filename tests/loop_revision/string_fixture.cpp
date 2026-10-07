#include "util/d_string.h"
#include <cstdlib>
#include <cstring>
extern const char nothing = '\0';
String::String(String const& other) {
	set(&other);
}
String& String::operator=(String const& other) {
	if (this != &other)
		set(&other);
	return *this;
}
String::~String() {
	clear();
}
void String::clear(bool) {
	std::free(stringMemory);
	stringMemory = nullptr;
}
Error String::set(char const* text, int32_t length) {
	if (length < 0)
		length = std::strlen(text);
	auto* next = static_cast<char*>(std::malloc(length + 1));
	if (!next)
		return Error::INSUFFICIENT_RAM;
	std::memcpy(next, text, length);
	next[length] = 0;
	clear();
	stringMemory = next;
	return Error::NONE;
}
void String::set(String const* other) {
	set(other->get());
}
