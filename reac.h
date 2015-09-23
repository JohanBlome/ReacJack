#ifndef REAC_H_INCLUDED
#define REAC_H_INCLUDED

#include <stdint.h>

const uint8_t REAC_ENDING[2] = { 0xc2, 0xea };
const uint8_t  REAC_PROTOCOL[2] = { 0x88, 0x19 };
typedef struct {
    uint8_t ENDING[2];
    uint8_t PROTOCOL[2];
} REACConstants;

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

const int MAX_INT32 = 1<<31;
const int MAX_INT24 = 1<<23;
const int MAX_INT16 = 1<<15;
#endif // REAC_H_INCLUDED
