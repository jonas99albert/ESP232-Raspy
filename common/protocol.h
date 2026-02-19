#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

/*
 * Gemeinsames Kommunikationsprotokoll zwischen RPi5 und ESP32-C3.
 *
 * Jede Nachricht besteht aus einem festen Header gefolgt von
 * optionalen Payload-Bytes. Alle Multi-Byte-Felder sind Little-Endian.
 *
 *  [ magic (2) | type (1) | device_id (1) | payload_len (2) | payload (N) ]
 */

#define PROTO_MAGIC        0xCAFE
#define PROTO_PORT         9000
#define PROTO_MAX_PAYLOAD  256

/* Nachrichtentypen */
typedef enum {
    MSG_HEARTBEAT   = 0x01,   /* ESP -> RPi: "Ich lebe noch"          */
    MSG_SENSOR_DATA = 0x02,   /* ESP -> RPi: Sensordaten senden       */
    MSG_COMMAND     = 0x03,   /* RPi -> ESP: Befehl an ESP senden     */
    MSG_ACK         = 0x04,   /* Empfangsbestaetigung                 */
    MSG_REGISTER    = 0x10,   /* ESP -> RPi: Geraet anmelden          */
} msg_type_t;

/* Fester Header (6 Bytes) */
typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint8_t  type;
    uint8_t  device_id;
    uint16_t payload_len;
} msg_header_t;

/* Beispiel-Payload: Sensordaten */
typedef struct __attribute__((packed)) {
    float temperature;
    float humidity;
} sensor_data_t;

/* Beispiel-Payload: Befehl */
typedef struct __attribute__((packed)) {
    uint8_t cmd_id;
    uint8_t params[16];
} command_t;

#endif /* PROTOCOL_H */
