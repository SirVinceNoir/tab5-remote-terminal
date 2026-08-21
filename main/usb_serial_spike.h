#pragma once

/*
 * Phase 2 spike: bring up the USB Host stack + CDC-ACM class driver on the
 * Tab5's USB-A host port and confirm enumeration + read/write against a
 * real USB-serial adapter. Auto-detects FTDI/CH34x/CP210x chips, falls back
 * to the generic CDC-ACM driver otherwise. Throwaway code -- logs to serial,
 * no persistence. Superseded by the real serial-console Session transport
 * in Phase 6.
 */
void usb_serial_spike_start(void);
