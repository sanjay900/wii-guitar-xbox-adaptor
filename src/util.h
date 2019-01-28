#pragma once

#define bit_set(p,m) ((p) |= _BV(m))
#define bit_clear(p,m) ((p) &= ~_BV(m))
#define bit_write(c,p,m) (c ? bit_set(p,m) : bit_clear(p,m))
#define bit_check(value, bit) (((value) >> _BV(bit)) & 0x01)
