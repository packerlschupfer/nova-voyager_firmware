/**
 * @file strings.h
 * @brief Centralized string constants
 *
 * Common error messages, status strings, and UI text
 * Reduces flash usage through string pooling
 */

#ifndef STRINGS_H
#define STRINGS_H

/*===========================================================================*/
/* System Messages                                                            */
/*===========================================================================*/

#define STR_OK              "OK"
#define STR_ERROR           "ERROR"
#define STR_FAILED          "FAILED"
#define STR_TIMEOUT         "TIMEOUT"
#define STR_INVALID         "INVALID"

/*===========================================================================*/
/* Status Messages                                                            */
/*===========================================================================*/

#define STR_MOTOR_STOPPED   "Motor STOPPED"
#define STR_MOTOR_RUNNING   "Motor RUNNING"
#define STR_MOTOR_FAULT     "Motor FAULT"
#define STR_JAM_DETECTED    "JAM DETECTED"
#define STR_GUARD_OPEN      "GUARD OPEN"
#define STR_GUARD_CLOSED    "GUARD CLOSED"
#define STR_ESTOP_ACTIVE    "E-STOP ACTIVE"
#define STR_ESTOP_CLEAR     "E-STOP CLEAR"

/*===========================================================================*/
/* Error Messages                                                             */
/*===========================================================================*/

#define STR_ERR_CHECKSUM    "CHECKSUM ERROR"
#define STR_ERR_FRAME       "FRAME ERROR"
#define STR_ERR_OVERFLOW    "BUFFER OVERFLOW"
#define STR_ERR_STACK       "STACK OVERFLOW"
#define STR_ERR_HARDFAULT   "HARD FAULT"
#define STR_ERR_MALLOC      "MALLOC FAILED"
#define STR_ERR_WATCHDOG    "WATCHDOG RESET"
#define STR_ERR_ADC         "ADC FAULT"
#define STR_ERR_SETTINGS    "SETTINGS CORRUPT"

/*===========================================================================*/
/* UART Messages                                                              */
/*===========================================================================*/

#define STR_NEWLINE         "\r\n"
#define STR_PROMPT          "> "
#define STR_UNKNOWN_CMD     "Unknown command"
#define STR_TYPE_HELP       "Type HELP for commands"

/*===========================================================================*/
/* Tapping Mode Names                                                         */
/*===========================================================================*/

#define STR_TAP_OFF         "OFF"
#define STR_TAP_PEDAL       "PEDAL"
#define STR_TAP_SMART       "SMART"
#define STR_TAP_DEPTH       "DEPTH"
#define STR_TAP_LOAD        "LOAD"
#define STR_TAP_PECK        "PECK"

/*===========================================================================*/
/* State Names                                                                */
/*===========================================================================*/

#define STR_STATE_STARTUP   "STARTUP"
#define STR_STATE_IDLE      "IDLE"
#define STR_STATE_DRILLING  "DRILLING"
#define STR_STATE_TAPPING   "TAPPING"
#define STR_STATE_MENU      "MENU"
#define STR_STATE_ERROR     "ERROR"

#endif // STRINGS_H
