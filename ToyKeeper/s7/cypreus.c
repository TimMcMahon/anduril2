/*
 * This is intended for use on flashlights with a clicky switch and off-time
 * memory capacitor.  Ideally, a triple XP-L powered by a BLF17DD in a Sinner
 * Cypreus tri-EDC host.  It's mostly based on JonnyC's STAR firmware and
 * ToyKeeper's tail-light firmware.
 *
 * Original author: JonnyC
 * Modifications: ToyKeeper / Selene Scriven
 *
 * NANJG 105C Diagram
 *           ---
 *         -|1  8|- VCC
 * mem cap -|2  7|- Voltage ADC
 *  Star 3 -|3  6|- PWM
 *     GND -|4  5|- Star 2
 *           ---
 *
 * CPU speed is 4.8Mhz without the 8x divider when low fuse is 0x75
 *
 * define F_CPU 4800000  CPU: 4.8MHz  PWM: 9.4kHz       ####### use low fuse: 0x75  #######
 *                             /8     PWM: 1.176kHz     ####### use low fuse: 0x65  #######
 * define F_CPU 9600000  CPU: 9.6MHz  PWM: 19kHz        ####### use low fuse: 0x7a  #######
 *                             /8     PWM: 2.4kHz       ####### use low fuse: 0x6a  #######
 * 
 * Above PWM speeds are for phase-correct PWM.  This program uses Fast-PWM,
 * which when the CPU is 4.8MHz will be 18.75 kHz
 *
 * FUSES
 *      I use these fuse settings
 *      Low:  0x75
 *      High: 0xff
 *
 * STARS (not used)
 *
 * VOLTAGE
 *      Resistor values for voltage divider (reference BLF-VLD README for more info)
 *      Reference voltage can be anywhere from 1.0 to 1.2, so this cannot be all that accurate
 *
 *           VCC
 *            |
 *           Vd (~.25 v drop from protection diode)
 *            |
 *          1912 (R1 19,100 ohms)
 *            |
 *            |---- PB2 from MCU
 *            |
 *          4701 (R2 4,700 ohms)
 *            |
 *           GND
 *
 *      ADC = ((V_bat - V_diode) * R2   * 255) / ((R1    + R2  ) * V_ref)
 *      125 = ((3.0   - .25    ) * 4700 * 255) / ((19100 + 4700) * 1.1  )
 *      121 = ((2.9   - .25    ) * 4700 * 255) / ((19100 + 4700) * 1.1  )
 *
 *      Well 125 and 121 were too close, so it shut off right after lowering to low mode, so I went with
 *      130 and 120
 *
 *      To find out what value to use, plug in the target voltage (V) to this equation
 *          value = (V * 4700 * 255) / (23800 * 1.1)
 *
 */
#define F_CPU 4800000UL

/*
 * =========================================================================
 * Settings to modify per driver
 */

#define VOLTAGE_MON                 // Comment out to disable
#define OWN_DELAY                   // Should we use the built-in delay or our own?

#define MODE_MOON           1       //
#define MODE_LOW            8       //
#define MODE_MED            39      //
#define MODE_HIGH           120     //
#define MODE_HIGHER         255     //
// If you change these, you'll probably want to change the "modes" array below
#define SOLID_MODES         5       // How many non-blinky modes will we have?
#define DUAL_BEACON_MODES   5+3     // How many beacon modes will we have (with background light on)?
#define SINGLE_BEACON_MODES 5+3+1   // How many beacon modes will we have (without background light on)?
#define FIXED_STROBE_MODES  5+3+1+3 // How many constant-speed strobe modes?
//#define VARIABLE_STROBE_MODES 5+3+1+3+2 // How many variable-speed strobe modes?
#define BATT_CHECK_MODE     5+3+1+3+1 // battery check mode index
// Note: don't use more than 32 modes, or it will interfere with the mechanism used for mode memory
#define TOTAL_MODES         BATT_CHECK_MODE

#define WDT_TIMEOUT         2       // Number of WTD ticks before mode is saved (.5 sec each)

//#define ADC_LOW             130     // When do we start ramping
//#define ADC_CRIT            120     // When do we shut the light off

#define ADC_42          185 // the ADC value we expect for 4.20 volts
#define VOLTAGE_FULL    169 // 3.9 V, 4 blinks
#define VOLTAGE_GREEN   154 // 3.6 V, 3 blinks
#define VOLTAGE_YELLOW  139 // 3.3 V, 2 blinks
#define VOLTAGE_RED     124 // 3.0 V, 1 blink
#define ADC_LOW         123 // When do we start ramping down
#define ADC_CRIT        113 // When do we shut the light off

// Values between 1 and 255 corresponding with cap voltage (0 - 1.1v) where we
// consider it a short press to move to the next mode Not sure the lowest you
// can go before getting bad readings, but with a value of 70 and a 1uF cap, it
// seemed to switch sometimes even when waiting 10 seconds between presses.
#define CAP_SHORT       130 // Above this is a "short" press
#define CAP_MED         90  // Above this is a "medium" press
                            // ... and anything below that is a "long" press
#define CAP_PIN         PB3
#define CAP_CHANNEL 0x03   // MUX 03 corresponds with PB3 (Star 4)
#define CAP_DIDR    ADC3D  // Digital input disable bit corresponding with PB3


/*
 * =========================================================================
 */

#ifdef OWN_DELAY
#include <util/delay_basic.h>
// Having own _delay_ms() saves some bytes AND adds possibility to use variables as input
static void _delay_ms(uint16_t n)
{
    // TODO: make this take tenths of a ms instead of ms,
    // for more precise timing?
    // (would probably be better than the if/else here for a special-case
    // sub-millisecond delay)
    if (n==0) { _delay_loop_2(300); }
    else {
        while(n-- > 0)
            _delay_loop_2(890);
    }
}
#else
#include <util/delay.h>
#endif

#include <avr/interrupt.h>
#include <avr/wdt.h>
#include <avr/eeprom.h>
#include <avr/sleep.h>

#define STAR2_PIN   PB0
#define STAR3_PIN   PB4
#define STAR4_PIN   PB3
#define PWM_PIN     PB1
#define VOLTAGE_PIN PB2
#define ADC_CHANNEL 0x01    // MUX 01 corresponds with PB2
#define ADC_DIDR    ADC1D   // Digital input disable bit corresponding with PB2
#define ADC_PRSCL   0x06    // clk/64

#define PWM_LVL     OCR0B   // OCR0B is the output compare register for PB1

/*
 * global variables
 */

// Mode storage
uint8_t eepos = 0;
uint8_t eep[32];
// change to 1 if you want on-time mode memory instead of "short-cycle" memory
#define memory 0
//uint8_t memory = 0;

// Modes (hardcoded to save space)
static uint8_t modes[TOTAL_MODES] = { // high enough to handle all
    MODE_MOON, MODE_LOW, MODE_MED, MODE_HIGH, MODE_HIGHER, // regular solid modes
    MODE_MOON, MODE_LOW, MODE_MED, // dual beacon modes (this level and this level + 2)
    MODE_HIGHER, // heartbeat beacon
    99, 41, 15, // constant-speed strobe modes (10 Hz, 24 Hz, 60 Hz)
    //MODE_HIGHER, MODE_HIGHER, // variable-speed strobe modes
    MODE_MED, // battery check mode
};
static uint8_t neg_modes[] = {
    SOLID_MODES-1,           // Turbo / "impress" mode
    FIXED_STROBE_MODES-2,    // 24Hz strobe
    BATT_CHECK_MODE-1,       // Battery check
};
volatile uint8_t mode_idx = 0;
// 1 or -1. Do we increase or decrease the idx when moving up to a higher mode?
// Is set by checking stars in the original STAR firmware, but that's removed to save space.
#define mode_dir 1

void store_mode_idx(uint8_t lvl) {  //central method for writing (with wear leveling)
    uint8_t oldpos=eepos;
    eepos=(eepos+1)&31;  //wear leveling, use next cell
    // Write the current mode
    EEARL=eepos;  EEDR=lvl; EECR=32+4; EECR=32+4+2;  //WRITE  //32:write only (no erase)  4:enable  2:go
    while(EECR & 2); //wait for completion
    // Erase the last mode
    EEARL=oldpos;           EECR=16+4; EECR=16+4+2;  //ERASE  //16:erase only (no write)  4:enable  2:go
}
inline void read_mode_idx() {
    eeprom_read_block(&eep, 0, 32);
    while((eep[eepos] == 0xff) && (eepos < 32)) eepos++;
    if (eepos < 32) mode_idx = eep[eepos];
    else eepos=0;
}

inline void next_mode() {
    mode_idx += mode_dir;
    if (mode_idx > (TOTAL_MODES - 1)) {
        // Wrap around
        mode_idx = 0;
    }
}

inline void prev_mode() {
    if ((0x40 > mode_idx) && (mode_idx > 0)) {
        mode_idx -= mode_dir;
    } else if ((mode_idx&0x3f) < sizeof(neg_modes)) {
        mode_idx = (mode_idx|0x40) + mode_dir;
    } else {
        mode_idx = 0;
    }
    /*
    if (mode_idx < -(sizeof(neg_modes))) {
        // Wrap around
        //mode_idx = TOTAL_MODES - 1;
        // Er, actually, just reset to first mode group
        mode_idx = 0;
        // FIXME: should change mode group instead
    }
    */
}

inline void WDT_on() {
    // Setup watchdog timer to only interrupt, not reset, every 500ms.
    cli();                          // Disable interrupts
    wdt_reset();                    // Reset the WDT
    WDTCR |= (1<<WDCE) | (1<<WDE);  // Start timed sequence
    WDTCR = (1<<WDTIE) | (1<<WDP2) | (1<<WDP0); // Enable interrupt every 500ms
    sei();                          // Enable interrupts
}

inline void WDT_off()
{
    cli();                          // Disable interrupts
    wdt_reset();                    // Reset the WDT
    MCUSR &= ~(1<<WDRF);            // Clear Watchdog reset flag
    WDTCR |= (1<<WDCE) | (1<<WDE);  // Start timed sequence
    WDTCR = 0x00;                   // Disable WDT
    sei();                          // Enable interrupts
}

inline void ADC_on() {
    DIDR0 |= (1 << ADC_DIDR);                           // disable digital input on ADC pin to reduce power consumption
    ADMUX  = (1 << REFS0) | (1 << ADLAR) | ADC_CHANNEL; // 1.1v reference, left-adjust, ADC1/PB2
    ADCSRA = (1 << ADEN ) | (1 << ADSC ) | ADC_PRSCL;   // enable, start, prescale
}

inline void ADC_off() {
    ADCSRA &= ~(1<<7); //ADC off
}

#ifdef VOLTAGE_MON
uint8_t get_voltage() {
    // Start conversion
    ADCSRA |= (1 << ADSC);
    // Wait for completion
    while (ADCSRA & (1 << ADSC));
    // See if voltage is lower than what we were looking for
    return ADCH;
}
#endif

ISR(WDT_vect) {
    static uint8_t ticks = 0;
    if (ticks < 255) ticks++;

    // FIXME: do a turbo step-down here
    /*
    // FIXME: this is short-cycle memory, remove it
    // (we use off-time memory instead)
    if (ticks == WDT_TIMEOUT) {
#if memory
        store_mode_idx(mode_idx);
#else
        // Reset the mode to the start for next time
        store_mode_idx(0);
#endif
    }
    */
}

int main(void)
{
    // All ports default to input, but turn pull-up resistors on for the stars
    // (not the ADC input!  Made that mistake already)
    // (stars not used)
    //PORTB = (1 << STAR2_PIN) | (1 << STAR3_PIN) | (1 << STAR4_PIN);

    // Set PWM pin to output
    DDRB = (1 << PWM_PIN);

    // Set timer to do PWM for correct output pin and set prescaler timing
    TCCR0A = 0x23; // phase corrected PWM is 0x21 for PB1, fast-PWM is 0x23
    TCCR0B = 0x01; // pre-scaler for timer (1 => 1, 2 => 8, 3 => 64...)

    // Determine what mode we should fire up
    // Read the last mode that was saved
    read_mode_idx();

    // Start up ADC for capacitor pin
    // disable digital input on ADC pin to reduce power consumption
    DIDR0 |= (1 << CAP_DIDR);
    // 1.1v reference, left-adjust, ADC3/PB3
    ADMUX  = (1 << REFS0) | (1 << ADLAR) | CAP_CHANNEL;
    // enable, start, prescale
    ADCSRA = (1 << ADEN ) | (1 << ADSC ) | ADC_PRSCL;

    // Wait for completion
    while (ADCSRA & (1 << ADSC));
    // Start again as datasheet says first result is unreliable
    ADCSRA |= (1 << ADSC);
    // Wait for completion
    while (ADCSRA & (1 << ADSC));
    if (ADCH > CAP_SHORT) {
        // Indicates they did a short press, go to the next mode
        next_mode(); // Will handle wrap arounds
        store_mode_idx(mode_idx);
    } else if (ADCH > CAP_MED) {
        // User did a medium press, go back one mode
        prev_mode(); // Will handle wrap arounds
        store_mode_idx(mode_idx);
    } else {
#if memory
        // Didn't have a short press, keep the same mode
#else
        // Reset to the first mode
        mode_idx = 0;
        store_mode_idx(mode_idx);
#endif
    }
    // Turn off ADC
    ADC_off();

    // Charge up the capacitor by setting CAP_PIN to output
    DDRB  |= (1 << CAP_PIN);  // Output
    PORTB |= (1 << CAP_PIN);  // High

    // Turn features on or off as needed
    #ifdef VOLTAGE_MON
    ADC_on();
    #else
    //ADC_off();  // was already off
    #endif
    ACSR   |=  (1<<7); //AC off

    // Enable sleep mode set to Idle that will be triggered by the sleep_mode() command.
    // Will allow us to go idle between WDT interrupts
    set_sleep_mode(SLEEP_MODE_IDLE);

    // enable turbo step-down timer
    WDT_on();

    //if (mode_idx < 0) {
    if (mode_idx & 0x40) {
        mode_idx = neg_modes[(mode_idx&0x3f)-1];
    }

    // Now just fire up the mode
    // FIXME: move this into the clause for solid modes?
    PWM_LVL = modes[mode_idx];

    uint8_t i = 0;
    uint8_t j = 0;
    uint8_t strobe_len = 0;
#ifdef VOLTAGE_MON
    uint8_t lowbatt_cnt = 0;
    uint8_t voltage;
    voltage = get_voltage();
    //uint8_t hold_pwm;
#endif

    while(1) {
        if(mode_idx < SOLID_MODES) { // Just stay on at a given brightness
            sleep_mode();
        } else if (mode_idx < DUAL_BEACON_MODES) { // two-level fast strobe pulse at about 1 Hz
            for(i=0; i<4; i++) {
                PWM_LVL = modes[mode_idx-SOLID_MODES+2];
                _delay_ms(5);
                PWM_LVL = modes[mode_idx];
                _delay_ms(65);
            }
            _delay_ms(720);
        } else if (mode_idx < SINGLE_BEACON_MODES) { // heartbeat flasher
            PWM_LVL = modes[SOLID_MODES-1];
            _delay_ms(1);
            PWM_LVL = 0;
            _delay_ms(249);
            PWM_LVL = modes[SOLID_MODES-1];
            _delay_ms(1);
            PWM_LVL = 0;
            _delay_ms(749);
        } else if (mode_idx < FIXED_STROBE_MODES) { // strobe mode, fixed-speed
            strobe_len = 1;
            if (modes[mode_idx] < 50) { strobe_len = 0; }
            PWM_LVL = modes[SOLID_MODES-1];
            _delay_ms(strobe_len);
            PWM_LVL = 0;
            _delay_ms(modes[mode_idx]);
        } /* else if (mode_idx == VARIABLE_STROBE_MODES-2) {
            // strobe mode, smoothly oscillating frequency ~7 Hz to ~18 Hz
            for(j=0; j<66; j++) {
                PWM_LVL = modes[SOLID_MODES-1];
                _delay_ms(1);
                PWM_LVL = 0;
                if (j<33) { strobe_len = j; }
                else { strobe_len = 66-j; }
                _delay_ms(2*(strobe_len+33-6));
            }
        } else if (mode_idx == VARIABLE_STROBE_MODES-1) {
            // strobe mode, smoothly oscillating frequency ~16 Hz to ~100 Hz
            for(j=0; j<100; j++) {
                PWM_LVL = modes[SOLID_MODES-1];
                _delay_ms(0); // less than a millisecond
                PWM_LVL = 0;
                if (j<50) { strobe_len = j; }
                else { strobe_len = 100-j; }
                _delay_ms(strobe_len+9);
            }
        } */ else if (mode_idx < BATT_CHECK_MODE) {
            uint8_t blinks = 0;
            // division takes too much flash space
            //voltage = (voltage-ADC_LOW) / (((ADC_42 - 15) - ADC_LOW) >> 2);
            voltage = get_voltage();
            if (voltage >= ADC_42) {
                blinks = 5;
            }
            else if (voltage > VOLTAGE_FULL) {
                blinks = 4;
            }
            else if (voltage > VOLTAGE_GREEN) {
                blinks = 3;
            }
            else if (voltage > VOLTAGE_YELLOW) {
                blinks = 2;
            }
            else if (voltage > ADC_LOW) {
                blinks = 1;
            }
            // turn off and wait one second before showing the value
            PWM_LVL = 0;
            _delay_ms(1000);
            // blink up to five times to show voltage
            // (~0%, ~25%, ~50%, ~75%, ~100%, >100%)
            for(i=0; i<blinks; i++) {
                PWM_LVL = MODE_MED;
                _delay_ms(100);
                PWM_LVL = 0;
                _delay_ms(400);
            }
            _delay_ms(1000);  // wait at least 1 second between readouts
        }
#ifdef VOLTAGE_MON
        if (ADCSRA & (1 << ADIF)) {  // if a voltage reading is ready
            voltage = get_voltage();
            // See if voltage is lower than what we were looking for
            if (voltage < ((mode_idx == 0) ? ADC_CRIT : ADC_LOW)) {
                ++lowbatt_cnt;
            } else {
                lowbatt_cnt = 0;
            }
            // See if it's been low for a while, and maybe step down
            if (lowbatt_cnt >= 3) {
                if (mode_idx > 0) {
                    mode_idx = 0;
                } else { // Already at the lowest mode
                    // Turn off the light
                    PWM_LVL = 0;
                    // Disable WDT so it doesn't wake us up
                    WDT_off();
                    // Power down as many components as possible
                    set_sleep_mode(SLEEP_MODE_PWR_DOWN);
                    sleep_mode();
                }
                lowbatt_cnt = 0;
                // Wait at least 1 second before lowering the level again
                _delay_ms(1000);  // this will interrupt blinky modes
            }

            // Make sure conversion is running for next time through
            ADCSRA |= (1 << ADSC);
        }
#endif
    }
}
