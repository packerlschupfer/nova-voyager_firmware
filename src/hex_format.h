// Simple hex formatting helper
static inline char hex_digit(uint8_t val) {
    return (val < 10) ? ('0' + val) : ('A' + val - 10);
}

static inline void format_hex_byte(char* buf, uint8_t byte) {
    buf[0] = hex_digit(byte >> 4);   // High nibble
    buf[1] = hex_digit(byte & 0x0F); // Low nibble
    buf[2] = '\0';
}
