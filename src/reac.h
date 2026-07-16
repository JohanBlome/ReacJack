#ifndef REAC_H_INCLUDED
#define REAC_H_INCLUDED

#include <stdint.h>

/* REAC packet header */
typedef struct {
    uint8_t counter[2];
    uint8_t type[2];
    uint8_t data[32];
}REACPacketHeader;

//Ethernet Header
typedef struct EthernetHeader
{
    uint8_t dest[6]; //Total 48 bits
    uint8_t source[6]; //Total 48 bits
    uint16_t type; //16 bits
}   ETHERHeader;

#define MAX_INT32 (1LL << 31)
#define MAX_INT24 (1 << 23)
#define MAX_INT16 (1 << 15)
#endif // REAC_H_INCLUDED
