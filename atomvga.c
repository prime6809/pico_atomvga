/*
 * Copyright (c) 2021 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include "pico.h"

#if (R65C02 == 1)
#include "r65c02.pio.h"
#else
#include "atomvga.pio.h"
#endif 

#include "atomvga_out.pio.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/scanvideo.h"
#include "pico/scanvideo/composable_scanline.h"
#include "pico/sync.h"
#include "hardware/irq.h"
#include "hardware/vreg.h"
#include "hardware/clocks.h"

#include "atomvga.h"
#include "fonts.h"
#include "platform.h"
#if (PLATFORM == PLATFORM_DRAGON)
#include "eeprom.h"
#endif


// PIA and frambuffer address moved into platform.h -- PHS


// #define vga_mode vga_mode_320x240_60
#define vga_mode vga_mode_640x480_60

const uint LED_PIN = 25;
const uint SEL1_PIN = test_PIN_SEL1;
const uint SEL2_PIN = test_PIN_SEL1 + 1;
const uint SEL3_PIN = test_PIN_SEL1 + 2;

static PIO pio = pio1;
//static uint8_t *fontdata = fontdata_6847;

static uint32_t vga80_lut[128 * 4];
static uint16_t pix8to16[256];

volatile uint8_t fontno = DEFAULT_FONT;
volatile uint8_t max_lower = LOWER_END;

volatile uint16_t ink = DEF_INK;
volatile uint16_t ink_alt = DEF_INK_ALT;  
volatile uint16_t paper = DEF_PAPER;

volatile uint16_t border = DEF_PAPER;

volatile uint8_t autoload = 0;

volatile uint8_t artifact = 0;

volatile bool reset_flag = false;

volatile uint8_t status = STATUS_NONE;
#define status_set(mask)    ((status & mask) ? true : false)
#define extmode_set(mask)   ((memory[COL80_BASE] & mask) ? true : false)

#define SetupIO(gpio,out,up,down)   do {if (gpio < 32) { gpio_init(gpio); gpio_set_dir(gpio,out); gpio_set_pulls(gpio,up,down);}; } while (0)

// Initialise the GPIO pins - overrides whatever the scanvideo library did
static void initialiseIO()
{
    // Grab the uart pins back from the video function
    gpio_set_function(0, GPIO_FUNC_UART);
    gpio_set_function(1, GPIO_FUNC_UART);

    // pins 2 to 9 are used to read the 6502 / 6809 bus - 8 bits at a time
    for (uint pin = 2; pin <= 9; pin++)
    {
        gpio_init(pin);
        gpio_set_dir(pin, false);
        gpio_set_function(pin, GPIO_FUNC_PIO1);
    }

    // Output enable for the 74lvc245 buffers
    gpio_pull_up(SEL1_PIN);
    gpio_pull_up(SEL2_PIN);
    gpio_pull_up(SEL3_PIN);

    gpio_set_function(SEL1_PIN, GPIO_FUNC_PIO1);
    gpio_set_function(SEL2_PIN, GPIO_FUNC_PIO1);
    gpio_set_function(SEL3_PIN, GPIO_FUNC_PIO1);

    // Set Phi2, E, R/W as inputs, *REQUIRED* for RP235x
    SetupIO(test_PIN_1MHZ,false,true,false);     // Phi2 on 6502, E on 6809.
    SetupIO(test_PIN_R_NW,false,true,false);     // R/W

    // LED
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
}

void reset_vga80();
void set_extmode(uint8_t    mode);
void core1_func();

static semaphore_t video_initted;

bool updated;

volatile uint8_t memory[0x10000];

#define CSI "\x1b["

void load_ee(void);

// Returns video mode as far as VDG is concerned, with the bits :
//  b3  b2  b1  b0
//  GM2 GM1 GM0 A/G
int get_mode()
{
#if (PLATFORM == PLATFORM_ATOM)
    return (memory[PIA_ADDR] & 0xf0) >> 4;
#elif (PLATFORM == PLATFORM_DRAGON)
    return ((memory[PIA_ADDR] & 0x80) >> 7) | ((memory[PIA_ADDR] & 0x70) >> 3);
#endif
}

inline bool alt_colour()
{
#if (PLATFORM == PLATFORM_ATOM)
    return !!(memory[PIA_ADDR + 2] & 0x8);
#elif (PLATFORM == PLATFORM_DRAGON)
    return (memory[PIA_ADDR] & 0x08);
#endif
}

inline bool is_artifact(uint mode)
{
    return (((0 != (status & STATUS_ARTI_MASK))) && (0x0F == mode)) ? true : false;
}

// Treat artifacted pmode 4 as pmode 3 with a different palette
inline bool is_colour(uint mode)
{
    return !(mode & 0b10);
};

const uint debug_text_len = 32;
char debug_text[33];
//bool debug = false;

void set_debug_text(char *text)
{
    strncpy(debug_text, text, debug_text_len);

    for (unsigned int i = strnlen(text, debug_text_len); i < debug_text_len; i++)
    {
        debug_text[i] = ' ';
    }

    for (unsigned int i = 0; i < debug_text_len; i++)
    {
        unsigned char c = debug_text[i];

#if (PLATFORM == PLATFORM_ATOM)
        c = c + 0x20;
        if (c < 0x80)
        {
            c = c ^ 0x60;
        }
#else
        if((c >= 0x20) && (c <= 0x3F))
        {
            c = c + 0x40;
        }
        else if ((c >= 0x60) && (c <= 0x7F))
        {
            c = c - 0x60;
        }
#endif
        debug_text[i] = c;
    }
}

#define DEBUG_BUF_SIZE  80

void update_debug_text()
{
    if (status_set(STATUS_DEBUG))
    {
        char buffer[DEBUG_BUF_SIZE];
        uint mode = get_mode();

        uint bytes = bytes_per_row(mode) * get_height(mode);

        uint m = (mode + 1) / 2;
#if (PLATFORM == PLATFORM_ATOM)
        snprintf(buffer, DEBUG_BUF_SIZE, "mode %x/%d %dx%d %s %d",
                mode,
                m,
                get_width(mode),
                get_height(mode),
                is_colour(mode) ? "col" : "b&w",
                bytes);
#elif (PLATFORM == PLATFORM_DRAGON)
        snprintf(buffer, DEBUG_BUF_SIZE, "mode %x/%d %dx%d %s %d %04X %02X",
                mode,
                m,
                get_width(mode),
                get_height(mode),
                is_colour(mode) ? "col" : "b&w",
                bytes,
                SAMBits,
                (status & STATUS_ARTI_MASK));
#endif
        set_debug_text(buffer);
    }
    else
    {
        set_debug_text("");
    }
}

inline void status_sc(bool      state,
                      uint8_t   bits)
{
    status = state ? (status | bits) : (status & ~bits); 
}



#if (PLATFORM == PLATFORM_ATOM)
bool is_command(char *cmd,
                char **params)
{
    char *p = (char *)memory + CMD_BASE;
    *params=(char *)NULL;   
    
    while (*cmd != 0)
    {
        if (*cmd++ != *p++)
        {
            return false;
        }
    }

    if ((ATOM_EOL == *p) || (SPACE == *p))
    {
        *params = p;
        return true;
    }
    return false;
}

bool uint8_param(char *params,
                 int *output,
                 int min,
                 int max)
{
    int try;
    if (sscanf(params, "%d", &try))
    {
        if ((try >= min) && (try <= max))
        {
            *output = try;
            return true;
        }
    }
    return false;
}

#endif

void switch_font(uint8_t new_font)
{
    uint8_t font_range;
 
    // make sure new fontno is valid.....
    fontno = (new_font < FONT_COUNT) ? new_font : DEFAULT_FONT;

    // Calculate range of available lower case symbols
    font_range = fonts[fontno].last_upper - fonts[fontno].first_upper;

    max_lower = (font_range < LOWER_RANGE) ? LOWER_START + font_range : LOWER_END;
}

void switch_colour(uint8_t          newcolour,
                   volatile uint16_t *tochange)
{
    if (newcolour < NO_COLOURS)
    {
        *tochange=colour_palette_vdg[newcolour];
    }
}

void switch_border(uint8_t          newcolour)
{
    if (newcolour < NO_COLOURS)
    {
        border=colour_palette_vdg[newcolour];
        status_sc(true,STATUS_BORDER);
    }
    else if (IDX_DEFAULTS == newcolour)
    {
        status_sc(false,STATUS_BORDER);
    }
}

void switch_palette(uint8_t          slots)
{
    uint8_t slot1 = (slots & 0xF0) >> 4; // slots / 16;
    uint8_t slot2 = slots & 0x0F;

    if ( ( slot1 < NO_COLOURS) && ( slot2 < NO_COLOURS)  )
    {
        colour_palette[slot1] = colour_palette_vdg_default[slot2];
    } 
    else if (IDX_DEFAULTS == slots) // Reset the palette from the default
    {
        for ( int i = 0; i < NO_COLOURS; ++i)
        {
            colour_palette[i] = colour_palette_vdg_default[i];
        }
    }
}

#if (R65C02 == 1)
void __no_inline_not_in_flash_func(main_loop())
{
    while (true)
    {
        // Get event from SM 0
        uint32_t reg = pio_sm_get_blocking(pio, 0);

        // Is it a read to the COL80 I/O space?
        if ((reg & (0x1000000 | COL80_MASK)) == COL80_BASE)
        {
            // read
            pio_sm_put(pio, 1, 0xFF00 | memory[reg]);
        }
        else if (reg & 0x1000000)
        {
            // write
            uint16_t address = reg & 0xFFFF;

            uint8_t data = (reg & 0xFF0000) >> 16;
            memory[address] = data;
        }
    }
}
#else

void __no_inline_not_in_flash_func(main_loop())
{
    static uint16_t    last = 0;

    while (true)
    {
        // Get event from SM 0
        uint32_t reg = pio_sm_get_blocking(pio, 0);

        // Is it a read or write opertaion?
        if (!(reg & 0x1000000))
        {
            // Get the address
            uint16_t address = reg;
            // read
            if (address == COL80_STAT)
            {
                uint8_t b = 0x12;
                pio_sm_put(pio, 1, 0xFF00 | b);
            }
            else if ((address & COL80_MASK) == COL80_BASE)
            {
                uint8_t b = memory[address];
                pio_sm_put(pio, 1, 0xFF00 | b);
            } 
            else if (address == STATUS_ADDR)
            {
                pio_sm_put(pio, 1, 0xFF00 | status);
            }
#if (PLATFORM == PLATFORM_DRAGON)
            else if ((address > STATUS_ADDR) && (address < DRAGON_PALETTE_ADDR))
            {
                pio_sm_put(pio, 1, 0xFF00 | memory[address]);
            }
#endif
            // Check for reset vector fetch, if so flag reset
            if ((RESET_VEC+1 == address) && (RESET_VEC == last))
            {
                reset_flag = true;
            }   
            last = address; 
        }
        else
        {
            // Get the address
            uint16_t address = reg & 0xFFFF;
            // write
            uint8_t data = (reg & 0xFF0000) >> 16;
            memory[address] = data;
#if (PLATFORM == PLATFORM_DRAGON)
            // Update SAM bits when written to
            if ((address >= SAM_BASE) && (address <= SAM_END))
            {
                uint8_t     sam_data = GetSAMData(address);
                uint16_t    sam_mask = GetSAMDataMask(address);

                if (sam_data)
                {
                    SAMBits |= sam_mask;
                }
                else
                {
                    SAMBits &= ~sam_mask;
                }
            }

            // Change the font as requested
            if (DRAGON_FONTNO_ADDR == address)
            {
                switch_font(data);
            } 
            else if (DRAGON_INK_ADDR == address)
            {
                switch_colour(data,&ink);
            }
            else if (DRAGON_PAPER_ADDR == address)
            {
                switch_colour(data,&paper);
            }
            else if (DRAGON_INKALT_ADDR == address)
            {
                switch_colour(data,&ink_alt);
            }
            else if (DRAGON_BORDER_ADDR == address)
            {
                switch_border(data);
            }
            else if (DRAGON_PALETTE_ADDR == address)
            {
                switch_palette(data);
            }
#endif
        }
    }
}
#endif

void print_str(int line_num, char* str)
{
    set_debug_text(str);
    memcpy((char *)(memory + GetVidMemBase() + 0x020*line_num), debug_text, 32);
}

int main(void)
{
    uint sys_freq = 250000;
    if (sys_freq > 250000)
    {
        vreg_set_voltage(VREG_VOLTAGE_1_25);
    }
    set_sys_clock_khz(sys_freq, true);

    stdio_init_all();
    printf("DragonVGA initialising........\n");

    switch_font(DEFAULT_FONT);

    memset((void *)memory, VDG_SPACE, 0x10000);

#if (PLATFORM == PLATFORM_DRAGON)
    ee_at_reset();
    load_ee();
#endif

    char mess[32];

    // Display message and build date/time
    print_str(4, DEBUG_MESS);
    print_str(5, __DATE__ " " __TIME__);
    snprintf(mess, 32, "BASE=%04X, PIA=%04X", GetVidMemBase(), PIA_ADDR);
    print_str(6, mess);
#if (R65C02 == 1)
    print_str(7, "R65C02 VERSION");
#endif

    // create a semaphore to be posted when video init is complete
    sem_init(&video_initted, 0, 1);

    // launch all the video on core 1, so it isn't affected by USB handling on core 0
    multicore_launch_core1(core1_func);

    // wait for initialization of video to be complete
    sem_acquire_blocking(&video_initted);

    //initialiseIO();

    printf("Init input PIO\n");
    uint offset = pio_add_program(pio, &test_program);
    test_program_init(pio, 0, offset);
    pio_sm_set_enabled(pio, 0, true);

    printf("Init output PIO\n");
    offset = pio_add_program(pio, &atomvga_out_program);
    atomvga_out_program_init(pio, 1, offset);
    pio_sm_set_enabled(pio, 1, true);

    printf("Start main loop\n");
    main_loop();
}

#if (PLATFORM == PLATFORM_ATOM)
void check_command()
{
    char    *params = (char *)NULL;
    int temp;

    if (is_command("DEBUG",&params))
    {
        status_sc(true,STATUS_DEBUG);
        ClearCommand();
    }
    else if (is_command("NODEBUG",&params))
    {
        status_sc(false,STATUS_DEBUG);
        ClearCommand();
    }
    else if (is_command("LOWER",&params))
    {
        status_sc(true,STATUS_LOWER);
        ClearCommand();
    }
    else if (is_command("NOLOWER",&params))
    {
        status_sc(true,STATUS_LOWER);
        ClearCommand();
    }
    else if (is_command("CHARSET",&params))
    {
        temp=fontno;
        if (uint8_param(params,&temp,0,FONT_COUNT-1))
        {
            switch_font(temp);
        }
        ClearCommand();
    }
    else if (is_command("FG",&params))
    {
        if (uint8_param(params,&temp,0,NO_COLOURS-1))
        {
            switch_colour(temp,&ink);        
        }
        ClearCommand();
    }
    else if (is_command("FGA",&params))
    {
        if (uint8_param(params,&temp,0,NO_COLOURS-1))
        {
            switch_colour(temp,&ink_alt);
        }
        ClearCommand();
    }
    else if (is_command("BG",&params))
    {
        if (uint8_param(params,&temp,0,NO_COLOURS-1))
        {
            switch_colour(temp,&paper);
        }
        ClearCommand();
    }
    else if (is_command("BORDER",&params))
    {
        if (uint8_param(params,&temp,0,IDX_DEFAULTS))
        {
            switch_border(temp);
        }
    }
    else if (is_command("PALSWAP",&params))
    {
        if (uint8_param(params,&temp,0,IDX_DEFAULTS))
        {
            switch_palette(temp);
        }
    }
    else if (is_command("ARTI",&params))
    {
        if (uint8_param(params,&temp,0,2))
        {
            status_sc(false,STATUS_ARTI_MASK); 
            
            switch (temp)
            {
                case 1  : status_sc(true,STATUS_ARTI1); break;
                case 2  : status_sc(true,STATUS_ARTI2); break;
            }
        }
        ClearCommand();
    }
    else if (is_command("80COL",&params))
    {
        status_sc(true,STATUS_EXTMODE);
        set_extmode(COL80_ON);
        ClearCommand();
    }
    else if (is_command("64COL",&params))
    {
        status_sc(true,STATUS_EXTMODE);
        set_extmode(COL64_ON);
        ClearCommand();
    }
    else if (is_command("80COL",&params))
    {
        status_sc(true,STATUS_EXTMODE);
        set_extmode(COL40_ON);
        ClearCommand();
    }
}
#elif (PLATFORM == PLATFORM_DRAGON)

uint8_t index_from_colour(uint16_t  colour)
{
    uint8_t colidx = IDX_DEFAULTS;

    for(uint8_t palidx=0; palidx <= MAX_COLOUR; palidx++)
    {
        if(colour == colour_palette[palidx])
        {
            colidx=palidx;
        }
    }

    return colidx;
}

void save_ee(void)
{
    if (0 <= write_ee(EE_ADDRESS,EE_AUTOLOAD,autoload))
    {
        write_ee(EE_ADDRESS,EE_FONTNO,fontno);
        write_ee_bytes(EE_ADDRESS,EE_INK,(uint8_t *)&ink,sizeof(ink));
        write_ee_bytes(EE_ADDRESS,EE_PAPER,(uint8_t *)&paper,sizeof(paper));
        write_ee_bytes(EE_ADDRESS,EE_INK_ALT,(uint8_t *)&ink_alt,sizeof(ink_alt));
        write_ee_bytes(EE_ADDRESS,EE_BORDER,(uint8_t *)&border,sizeof(border));
        write_ee(EE_ADDRESS,EE_STATUS,status);

        write_ee_bytes(EE_ADDRESS,EE_COLOUR0,(uint8_t *)&colour_palette_vdg,sizeof(colour_palette_vdg));

        printf("fontno=%02X, ink=%04X, paper=%04X, alt_ink=%04X, status=%02X\n",fontno,ink,paper,ink_alt,status);
    }
}

void load_ee(void)
{
    uint8_t fontno;
    uint8_t autoload;

    if(0 <= read_ee(EE_ADDRESS,EE_AUTOLOAD,(uint8_t *)&autoload))
    {
        read_ee(EE_ADDRESS,EE_FONTNO,&fontno);
        switch_font(fontno);

        read_ee_bytes(EE_ADDRESS,EE_INK,(uint8_t *)&ink,sizeof(ink));
        read_ee_bytes(EE_ADDRESS,EE_PAPER,(uint8_t *)&paper,sizeof(paper));
        read_ee_bytes(EE_ADDRESS,EE_INK_ALT,(uint8_t *)&ink_alt,sizeof(ink_alt));
        read_ee_bytes(EE_ADDRESS,EE_BORDER,(uint8_t *)&border,sizeof(border));
        read_ee(EE_ADDRESS,EE_STATUS,(uint8_t *)&status);
        
        read_ee_bytes(EE_ADDRESS,EE_COLOUR0,(uint8_t *)&colour_palette_vdg,sizeof(colour_palette_vdg));

        // clear 80 col bit.
        status_sc(false,STATUS_EXTMODE);

        // update in memory copies of loaded defaults so that we can read them from basic
        memory[DRAGON_FONTNO_ADDR]=fontno;
        memory[DRAGON_INK_ADDR]=index_from_colour(ink);
        memory[DRAGON_PAPER_ADDR]=index_from_colour(paper);
        memory[DRAGON_INKALT_ADDR]=index_from_colour(ink_alt);
        memory[DRAGON_BORDER_ADDR]=index_from_colour(border);
        
        printf("fontno=%02X, ink=%04X, paper=%04X, alt_ink=%04X, status=%02X\n",fontno,ink,paper,ink_alt,status);
    }
}

void set_auto(uint8_t state)
{
    status_sc(state, STATUS_AUTO);

    write_ee(EE_ADDRESS,EE_AUTOLOAD,state);
}

// reset everything to it's default, and update eeprom.
void reset_all()
{
    // reset colours.
    ink = DEF_INK;
    paper = DEF_PAPER;
    border = DEF_PAPER;
    ink_alt = DEF_INK_ALT;

    // reset font
    fontno = DEFAULT_FONT_NO;

    // reset pallate
    memcpy((uint8_t *)&colour_palette_vdg,(uint8_t *)&colour_palette_vdg_default,sizeof(colour_palette_vdg));

    // Clear status bits.
    status_sc(false,STATUS_EXTMODE);    // 80 col off
    status_sc(false,STATUS_DEBUG);      // debug off
    status_sc(false,STATUS_LOWER);      // lower case off
    status_sc(false,STATUS_ARTI_MASK);  // artiface off
    
    // autoload on.
    status_sc(true,STATUS_AUTO);    

    // save then reload eeprom
    save_ee();
    load_ee();
}

void check_command()
{
    static uint8_t oldcommand = DRAGON_CMD_NOP;
    uint8_t command = memory[DRAGON_CMD_ADDR];

    if(command != oldcommand)
    {
        switch (command)
        {
            case DRAGON_CMD_NOP     : break;  
            case DRAGON_CMD_DEBUG   : status_sc(true,STATUS_DEBUG); break;
            case DRAGON_CMD_NODEBUG : status_sc(false,STATUS_DEBUG); break;
            case DRAGON_CMD_LOWER   : status_sc(true,STATUS_LOWER); break;
            case DRAGON_CMD_NOLOWER : status_sc(false,STATUS_LOWER); break;
            case DRAGON_CMD_ARTIOFF : status_sc(false,STATUS_ARTI_MASK); break;
            case DRAGON_CMD_ARTI1   : status_sc(false,STATUS_ARTI_MASK); status_sc(true,STATUS_ARTI1); break;
            case DRAGON_CMD_ARTI2   : status_sc(false,STATUS_ARTI_MASK); status_sc(true,STATUS_ARTI2); break;
            case DRAGON_CMD_SAVEEE  : save_ee(); break;
            case DRAGON_CMD_LOADEE  : load_ee(); break;
            case DRAGON_CMD_AUTOOFF : set_auto(AUTO_OFF); break;
            case DRAGON_CMD_AUTOON  : set_auto(AUTO_ON); break;
        }

        oldcommand=command;
    }
    // Update 80 column status.
    status_sc(extmode_set(EXTMODES),STATUS_EXTMODE);
}
#endif

void check_reset(void)
{
    if (reset_flag)
    {
        // back to 32 column mode
        reset_vga80();
        
        // reset colours if ink and paper are the same
        if(ink == paper)
        {
            ink = DEF_INK;
            paper = DEF_PAPER;
            border = DEF_PAPER;
            ink_alt = DEF_INK_ALT;
        }

        reset_flag = false;
    }
}

const uint vga_width = 640;
const uint vga_height = 480;

const uint max_width = 512;
const uint max_height = 384;

const uint vertical_offset = (vga_height - max_height) / 2;
const uint horizontal_offset = (vga_width - max_width) / 2;

const uint debug_start = max_height + vertical_offset; 

const uint vertical_offset80 = (vga_height - COL80_LINES) / 2;
const uint vertical_max80    = vertical_offset80 + COL80_LINES; 


uint16_t *add_border(uint16_t *p, uint16_t border_colour, uint16_t len)
{
    *p++ = COMPOSABLE_COLOR_RUN;
    *p++ = border_colour;
    *p++ = len - 3;
    return p;
}

// Process Text mode, Semigraphics modes.
//
// 6847 control Atom    Dragon
// INV          D7      D6
// !A/G         PA4     PB7
// !A/S         D6      D7
// CSS          PC3     PB3
// !INT/EXT     D6      PB4
// GM0          PA5     PB4
// GM1          PA6     PB5
// GM2          PA7     PB6
//
// Due to the interaction between the SAM and the VDG on the Dragon, there are 3
// extra semigraphics modes, SG8, SG12 and SG24, that split the character space up
// into 8, 12 and 24 pixels. These pixels are still half a character width but are
// 3, 2 and 1 scanlines high respectively.
// This happens by programming the VDG in text mode and the SAM in graphics mode.
// The memory used for these modes is incresed so that each vertical part of the
// character space is 32 bytes apart in memory.
// For example in the SG8 mode, the top 2 pixels are encoded in the first byte
// the next 2 in the second byte and so on.
// In this way the bytes per character line are 128, 192, and 384.
//
// Mode     Bytes/line  Bytes/chars colours resolution  memory
// SG4      32          1           8       64x32       512
// SG6      32          1           4       64x48       512
// SG8      128         4           8       64x64       2048
// SG12     192         6           8       64x96       3072
// SG24     384         12          8       64x192      6144
//
// In SG8, SG12, SG24, the byte format is the same as the byte format for SG4
// However if ralative_line_no is < 6 pixels 2 and 3 are plotted. If it is > 6
// pixels 0 and 1 are plotted.
//

// Changed parameter memory to be called vdu_base to avoid clash with global memory -- PHS
uint16_t *do_text(scanvideo_scanline_buffer_t *buffer, uint relative_line_num, char *vdu_base, uint16_t *p, bool is_debug)
{
    // Screen is 16 rows x 32 columns
    // Each char is 12 x 8 pixels
    // Note we divide ralative_line_number by 2 as we are double scanning each 6847 line to
    // 2 VGA lines.
    bool is_64col   = extmode_set(COL64_ON);                        // Flag to select 32 / 64 characters / row mode
    uint rowdiv     = is_64col ? 1 : 2;                             // row divider
    uint row = (relative_line_num / rowdiv) / FONT_HEIGHT;          // char row
    uint sub_row = (relative_line_num / rowdiv) % FONT_HEIGHT;      // scanline within current char row
    uint sgidx = is_debug ? TEXT_INDEX : GetSAMSG();                // index into semigraphics table
    uint rows_per_char  = FONT_HEIGHT / sg_bytes_row[sgidx];        // bytes per character space vertically
    uint8_t *fontdata = fonts[fontno].fontdata + sub_row;           // Local fontdata pointer
    uint cols = is_64col ? 64 : 32;                                 // Characters per row (columns)
    uint rows = is_64col ? 32 : 16;                                 // Rows

    if (row < rows)
    {
        // Calc start address for this row
        uint vdu_address = ((cols * sg_bytes_row[sgidx]) * row) + (cols * (sub_row / rows_per_char));

        for (uint col = 0; col < cols; col++)
        {
            // Get character data from RAM and extract inv,ag,int/ext
            uint ch = vdu_base[vdu_address + col];
            bool inv    = (ch & INV_MASK) ? true : false;
            bool as     = (ch & AS_MASK) ? true : false;
            bool intext = GetIntExt(ch);

            uint16_t fg_colour;
            uint16_t bg_colour = paper;

            // Deal with text mode first as we can decide this purely on the setting of the
            // alpha/semi bit.
            if(!as)
            {
                uint8_t b = fontdata[(ch & 0x3f) * FONT_HEIGHT];

                fg_colour = alt_colour() ? ink_alt : ink;

                if (status_set(STATUS_LOWER) && ch >= LOWER_START && ch <= max_lower)
                {
                    b = fontdata[((ch & 0x3f) + 64) * FONT_HEIGHT];

                    if (LOWER_INVERT)
                    {
                        bg_colour = fg_colour;
                        fg_colour = paper;    
                    }
                }
                else if (inv)
                {
                    bg_colour = fg_colour;
                    fg_colour = paper;
                }

                if (b == 0)
                {
                    *p++ = COMPOSABLE_COLOR_RUN;
                    *p++ = bg_colour;
                    *p++ = is_64col ? 8 - 3 : 16 - 3;
                }
                else
                {
                    // The internal character generator is only 6 bits wide, however external
                    // character ROMS are 8 bits wide so we must handle them here
                    uint16_t c = (b & 0x80) ? fg_colour : bg_colour;
                    *p++ = COMPOSABLE_RAW_RUN;
                    *p++ = c;
                    *p++ = is_64col ? 8 - 3 : 16 - 3;
                    if (!is_64col)
                    {
                            *p++ = c;
                    }

                    for (uint8_t mask = 0x40; mask > 0; mask = mask >> 1)
                    {
                        c = (b & mask) ? fg_colour : bg_colour;
                        *p++ = c;
                        if (!is_64col)
                        {
                            *p++ = c;
                        }
                    }
                }
            }
            else        // Semigraphics
            {
                uint colour_index;

                if (as && intext)
                {
                    sgidx = SG6_INDEX;           // SG6
                }

                colour_index = (SG6_INDEX == sgidx) ? (ch & SG6_COL_MASK) >> SG6_COL_SHIFT :  (ch & SG4_COL_MASK) >> SG4_COL_SHIFT;

                if (alt_colour() && (SG6_INDEX == sgidx))
                {
                    colour_index += 4;
                }

                fg_colour = colour_palette_vdg[colour_index];
            
                uint pix_row = (SG6_INDEX == sgidx) ? 2 - (sub_row / 4) : 1 - (sub_row / 6);

                uint16_t pix0 = ((ch >> (pix_row * 2)) & 0x1) ? fg_colour : bg_colour;
                uint16_t pix1 = ((ch >> (pix_row * 2)) & 0x2) ? fg_colour : bg_colour;
                *p++ = COMPOSABLE_COLOR_RUN;
                *p++ = pix1;
                *p++ = is_64col ? 4 - 3 : 8 - 3;
                *p++ = COMPOSABLE_COLOR_RUN;
                *p++ = pix0;
                *p++ = is_64col ? 4 - 3 : 8 - 3;
            }
        }
    }
    return p;
}

void draw_color_bar(scanvideo_scanline_buffer_t *buffer)
{
    const uint mode = get_mode();
    const uint line_num = scanvideo_scanline_number(buffer->scanline_id);
    uint16_t *p = (uint16_t *)buffer->data;
    int relative_line_num = line_num - vertical_offset;
    uint16_t *art_palette = (1 == artifact) ? colour_palette_artifact1 : colour_palette_artifact2; 

    if (line_num == 0)
    {
        check_command();
        update_debug_text();
        check_reset();
    }

    uint16_t *palette = colour_palette;

    // grab this now, before the palette gets shifted
    uint16_t bg = palette[8];

    if (alt_colour())
    {
        palette += 4;
    }

    // Graphics modes have a coloured border, text modes have a black border
    uint16_t border_colour = (mode & 1) ? palette[0] : 0;
    
    // Unless overriden
	border_colour = (status_set(STATUS_BORDER)) ? border : border_colour;

    uint debug_end = status_set(STATUS_DEBUG) ? debug_start + 24 : debug_start;

    if (relative_line_num < 0 || line_num >= debug_end)
    {
        // Add top/bottom borders
        p = add_border(p, border_colour, vga_width);
    }
    else
    {

        // Add left border
        p = add_border(p, border_colour, horizontal_offset - 1);

        if (line_num >= debug_start && line_num < debug_end)    // Debug in 'text' mode
        {
            p = do_text(buffer, line_num - debug_start, debug_text, p, true);
        }
        else if (!(mode & 1))                                   // Alphanumeric or Semigraphics
        {
            if (relative_line_num >= 0 && relative_line_num < (16 * 24))
            {
                p = do_text(buffer, relative_line_num, (char *)memory + GetVidMemBase(), p, false);
            }
        }
        else                                                    // Grapics modes
        {
            const int height = get_height(mode);
            relative_line_num = (relative_line_num / 2) * height / 192;
            if (relative_line_num >= 0 && relative_line_num < height)
            {
                uint vdu_address = GetVidMemBase() + bytes_per_row(mode) * relative_line_num;
                uint32_t *bp = (uint32_t *)memory + vdu_address / 4;

                *p++ = COMPOSABLE_RAW_RUN;
                *p++ = border_colour;
                *p++ = 512 + 1 - 3;

                const uint pixel_count = get_width(mode);

                if (is_colour(mode))
                {
                    uint32_t word = 0;
                    for (uint pixel = 0; pixel < pixel_count; pixel++)
                    {
                        if ((pixel % 16) == 0)
                        {
                            word = __builtin_bswap32(*bp++);
                        }
                        uint x = (word >> 30) & 0b11;
                        uint16_t colour=palette[x];
                        if (pixel_count == 128)
                        {
                            *p++ = colour;
                            *p++ = colour;
                            *p++ = colour;
                            *p++ = colour;
                        }
                        else if (pixel_count == 64)
                        {
                            *p++ = colour;
                            *p++ = colour;
                            *p++ = colour;
                            *p++ = colour;
                            *p++ = colour;
                            *p++ = colour;
                            *p++ = colour;
                            *p++ = colour;
                        }
                        word = word << 2;
                    }
                }
                else
                {
                    uint16_t fg = palette[0];
                    for (uint i = 0; i < pixel_count / 32; i++)
                    {
                        const uint32_t b = __builtin_bswap32(*bp++);
                        if (pixel_count == 256)
                        {
                            if (0 == artifact)
                            {    
                                for (uint32_t mask = 0x80000000; mask > 0;)
                                {
                                    uint16_t colour = (b & mask) ? fg : bg;
                                    *p++ = colour;
                                    *p++ = colour;
                                    mask = mask >> 1;

                                    colour = (b & mask) ? fg : bg;
                                    *p++ = colour;
                                    *p++ = colour;
                                    mask = mask >> 1;

                                    colour = (b & mask) ? fg : bg;
                                    *p++ = colour;
                                    *p++ = colour;
                                    mask = mask >> 1;

                                    colour = (b & mask) ? fg : bg;
                                    *p++ = colour;
                                    *p++ = colour;
                                    mask = mask >> 1;
                                }
                            }
                            else
                            {
                                uint32_t word = b;
                                for (uint apixel=0; apixel < 16; apixel++)
                                {
                                    uint acol = (word >> 30) & 0b11;
                                    uint16_t colour = art_palette[acol];
                                    
                                    *p++ = colour;
                                    *p++ = colour;
                                    *p++ = colour;
                                    *p++ = colour;
                                
                                    word = word << 2;
                                }
                            }
                        }

                        else if (pixel_count == 128)
                        {
                            for (uint32_t mask = 0x80000000; mask > 0; mask = mask >> 1)
                            {
                                uint16_t colour = (b & mask) ? palette[0] : bg;
                                *p++ = colour;
                                *p++ = colour;
                                *p++ = colour;
                                *p++ = colour;
                            }
                        }
                        else if (pixel_count == 64)
                        {
                            for (uint32_t mask = 0x80000000; mask > 0; mask = mask >> 1)
                            {
                                uint16_t colour = (b & mask) ? palette[0] : bg;
                                *p++ = colour;
                                *p++ = colour;
                                *p++ = colour;
                                *p++ = colour;
                                *p++ = colour;
                                *p++ = colour;
                                *p++ = colour;
                                *p++ = colour;
                            }
                        }
                    }
                }
            }
        }

        // Add right border
        p = add_border(p, border_colour, horizontal_offset);
    }

    // black pixel to end line
    *p++ = COMPOSABLE_RAW_1P;
    *p++ = 0;
    // end of line with alignment padding
    if (!(3u & (uintptr_t)p))
    {
        *p++ = COMPOSABLE_EOL_SKIP_ALIGN;
    }
    else
    {
        *p++ = COMPOSABLE_EOL_ALIGN;
    }
    *p++ = 0;

    buffer->data_used = ((uint32_t *)p) - buffer->data;
    assert(buffer->data_used < buffer->data_max);

    buffer->status = SCANLINE_OK;
}

void reset_vga80()
{
    memory[COL80_BASE] = COL80_OFF;     // Normal text mode (vga80 off)
    memory[COL80_FG] = IDX80_GREEN;            // Foreground Green
    memory[COL80_BG] = IDX80_BLACK;            // Background Black
    memory[COL80_STAT] = 0x12;
}

uint16_t double_bits(uint8_t    inbits)
{
    uint16_t result = 0;
    uint8_t  inmask = 0x01;
    uint16_t outmask = 0x0003;

    for(inmask = 0x01; inmask!=0; inmask = inmask << 1)
    {
        result = (inbits & inmask) ? result | outmask : result;
        outmask = outmask << 2;
    }

    return result;
}

void initialize_vga80()
{
    // Reset the VGA80 hardware
    reset_vga80();
    // Use an LUT to allow two pixels to be calculated at once, taking account of the attribute byte for colours
    //
    // Bit  8  7  6  5  4  3  2  1   0
    //      --bgc--  x  --fgc--  p1 p0
    //
    for (int i = 0; i < 128 * 4; i++)
    {
        vga80_lut[i]  = ((i & 1) ? colour_palette_vga80[(i >> 2) & 7] : colour_palette_vga80[(i >> 6) & 7]) << 16;
        vga80_lut[i] |= ((i & 2) ? colour_palette_vga80[(i >> 2) & 7] : colour_palette_vga80[(i >> 6) & 7]);
    }

    for (int i=0; i<256; i++)
    {
        pix8to16[i]=double_bits(i);
    }
}

static inline uint32_t *begin_row80(uint16_t *data,
                                    uint32_t pixels)
{
    *data++ = COMPOSABLE_RAW_RUN;
    *data++ = (uint16_t) (pixels & 0x0000FFFF);
    *data++ = (uint16_t) 640 - 3;
    *data++ = (uint16_t) (pixels >> 16);
    
    return (uint32_t *)data;
}

uint16_t *do_text_vga80(scanvideo_scanline_buffer_t *buffer, uint relative_line_num, uint16_t *p)
{
    // Screen is 80 columns by 40 rows
    // Each char is 12 x 8 pixels
    int row = relative_line_num / FONT_HEIGHT;
    uint sub_row = relative_line_num % FONT_HEIGHT;

    uint8_t *fontdata = fonts[fontno].fontdata + sub_row;
    uint8_t *fontdata_sg = fontdata_sg4 + sub_row;

    uint8_t cols = (memory[COL80_BASE] & COL40_ON) ? 40 : 80;

    if (row < COL80_CLINES)
    {
        // Compute the start address of the current row in the framebuffer
        volatile uint8_t *char_addr = memory + GetVidMemBase() + cols * row;

        // Read the VGA80 control registers
        uint vga80_fg = memory[COL80_FG];
        uint vga80_bg = memory[COL80_BG];
        uint vga80_base  = memory[COL80_BASE];

# if 0
        *p++ = COMPOSABLE_RAW_RUN;
        *p++ = BLACK;       // Extra black pixel
        *p++ = 642 - 3;     //
        *p++ = BLACK;       // Extra black pixel
#endif
        // For efficiency, compute two pixels at a time using a lookup table
        // p is now on a word boundary due to the extra pixels above
        uint32_t *q = (uint32_t *)p;

        if (vga80_base & 0x08)
        {
            // Attribute mode enabled, attributes follow the characters in the frame buffer
            volatile uint8_t *attr_addr = char_addr + 80 * 40;
            uint shift = (sub_row >> 1) & 0x06; // 0, 2 or 4
            // Compute these outside of the for loop for efficiency
            uint smask0 = 0x10 >> shift;
            uint smask1 = 0x20 >> shift;
            uint ulmask = (sub_row == 10) ? 0xFF : 0x00;
            for (int col = 0; col < 80; col++)
            {
                uint ch = *char_addr++;
                uint attr = *attr_addr++;
                uint32_t *vp = vga80_lut + ((attr & 0x77) << 2);
                if (attr & 0x80)
                {
                    // Semi Graphics
                    uint32_t p1 = (ch & smask1) ? *(vp + 3) : *vp;
                    uint32_t p0 = (ch & smask0) ? *(vp + 3) : *vp;
                    // Unroll the writing of the four pixel pairs
                    if (0 == col)
                    {
                        q=begin_row80(p,p1);
                    }
                    else
                    {
                        *q++ = p1;        
                    }
                    *q++ = p1;
                    *q++ = p0;
                    *q++ = p0;
                }
                else
                {
#if (PLATFORM == PLATFORM_DRAGON)
                    ch ^= 0x60;
#endif                    
                    // Text
                    uint8_t b = fontdata[(ch & 0x7f) * FONT_HEIGHT];
                    if (ch >= 0x80)
                    {
                        b = ~b;
                    }
                    // Underlined
                    if (attr & 0x08)
                    {
                        b |= ulmask;
                    }
                    // Unroll the writing of the four pixel pairs
                    if (0 == col)
                    {
                        q=begin_row80(p,*(vp + ((b >> 6) & 3)));        
                    }
                    else
                    {
                        *q++ = *(vp + ((b >> 6) & 3));
                    }
                    *q++ = *(vp + ((b >> 4) & 3));
                    *q++ = *(vp + ((b >> 2) & 3));
                    *q++ = *(vp + ((b >> 0) & 3));
                }
            }
        }
        else
        {
            // Attribute mode disabled, use default colours from the VGA80 control registers:
            //   bits 2..0 of vga80_fg (#BDE4) are the default foreground colour
            //   bits 2..0 of vga80_bg (#BDE5) are the default background colour
            uint attr = ((vga80_bg & 7) << 4) | (vga80_fg & 7);
            uint32_t *vp = vga80_lut + (attr << 2);
            uint8_t pixels;

            for (int col = 0; col < cols; col++)
            {
                uint8_t ch     = *char_addr++;
                // Get inverse and alpha/semi bits from char
                bool inv    = (ch & INV_MASK) ? true : false;
                bool as     = (ch & AS_MASK) ? true : false;

                // determine fg colour based on defaults if ASCII 
                // else based on character if semigraphics, pull
                // pixel data out of font
                if (!as) // ASCII
                {
                    attr = ((vga80_bg & 7) << 4) | (vga80_fg & 7);
                    pixels = fontdata[(ch & 0x3f) * FONT_HEIGHT];            

                    // Deal with lower case / invert
                    if (status_set(STATUS_LOWER) && ch >= LOWER_START && ch <= max_lower)
                    {
                        pixels = fontdata[((ch & 0x3f) + 64) * FONT_HEIGHT];

                        if (LOWER_INVERT)
                        {
                            pixels = ~pixels;
                        }
                    }
                    else if (inv)
                    {
                        pixels = ~pixels;
                    }
                }
                else // Semigraphics
                {
                    attr = ((vga80_bg & 7) << 4) | vdgpal_to_80colpal[(ch & SG4_COL_MASK) >> SG4_COL_SHIFT];
                    pixels = fontdata_sg[(ch & SG4_PAT_MASK) * FONT_HEIGHT];
                }
                vp = (vga80_lut + (attr << 2));
                
                if (80 == cols)
                {
                    // Unroll the writing of the four pixel pairs
                    if (0 == col)
                    {
                        q=begin_row80(p,*(vp + ((pixels >> 6) & 3)));
                    }
                    else
                    {
                        *q++ = *(vp + ((pixels >> 6) & 3));                    
                    }
                    *q++ = *(vp + ((pixels >> 4) & 3));
                    *q++ = *(vp + ((pixels >> 2) & 3));
                    *q++ = *(vp + ((pixels >> 0) & 3));
                }
                else // 40 column mode, write each twice....
                {
                    uint16_t pixels16 = pix8to16[pixels];

                    // Unroll the writing of the four pixel pairs
                    if (0 == col)
                    {
                        q=begin_row80(p,*(vp + ((pixels16 >> 14) & 3)));
                    }
                    else
                    {
                        *q++ = *(vp + ((pixels16 >> 14) & 3));
                    }
                    *q++ = *(vp + ((pixels16 >> 12) & 3));
                    *q++ = *(vp + ((pixels16 >> 10) & 3));
                    *q++ = *(vp + ((pixels16 >> 8) & 3));
                    *q++ = *(vp + ((pixels16 >> 6) & 3));
                    *q++ = *(vp + ((pixels16 >> 4) & 3));
                    *q++ = *(vp + ((pixels16 >> 2) & 3));
                    *q++ = *(vp + ((pixels16 >> 0) & 3));
                }
            }
        }
    }
    // The above loops add 80 x 4 = 320 32-bit words, which is 640 16-bit words plus 4 further 16 bit words for the COMPOSABLE_RAW_1P_SKIP_ALIGN,0 and COMPOSABLE_RAW_RUN, RL at line start
    return p + 640 + 2;
#if 0
    // The above loops add 80 x 4 = 320 32-bit words, which is 640 16-bit words
    return p + 640;
#endif
}

void draw_color_bar_vga80(scanvideo_scanline_buffer_t *buffer)
{
    const uint line_num = scanvideo_scanline_number(buffer->scanline_id);
    uint16_t border_colour = 0;
    uint16_t *p = (uint16_t *)buffer->data;
    
    if ((line_num < vertical_offset80) || (line_num > vertical_max80))
    {
        p = add_border(p, border_colour, vga_width);
    }
    else
    {
        p = do_text_vga80(buffer, line_num - vertical_offset80, p);
    }

    if (line_num == 0)
    {
        check_command();
        update_debug_text();
        check_reset();
    }

    // black pixel to end line
    *p++ = COMPOSABLE_RAW_1P;
    *p++ = 0;
    // end of line with alignment padding
    if (!(3u & (uintptr_t)p))
    {
        *p++ = COMPOSABLE_EOL_SKIP_ALIGN;
    }
    else
    {
        *p++ = COMPOSABLE_EOL_ALIGN;
    }
    *p++ = 0;

    buffer->data_used = ((uint32_t *)p) - buffer->data;
    assert(buffer->data_used < buffer->data_max);

    buffer->status = SCANLINE_OK;
}

// Set extended text mode, ensuring mode is valid and only one extended mode bit is
// enabled at once.
void set_extmode(uint8_t    mode)
{
    if ((COL80_ON == mode ) || (COL64_ON == mode ) || (COL40_ON == mode ))
    {
        memory[COL80_BASE] = (memory[COL80_BASE] & ~EXTMODES) | mode;
    }
}

void core1_func()
{
    // initialize video and interrupts on core 1
    initialize_vga80();
    scanvideo_setup(&vga_mode);
    initialiseIO();
    scanvideo_timing_enable(true);
    sem_release(&video_initted);
    while (true)
    {
        uint vga80 = memory[COL80_BASE] & (COL80_ON | COL40_ON);
        scanvideo_scanline_buffer_t *scanline_buffer = scanvideo_begin_scanline_generation(true);
        if (vga80)
        {
            draw_color_bar_vga80(scanline_buffer);
        }
        else
        {
            draw_color_bar(scanline_buffer);
        }
        scanvideo_end_scanline_generation(scanline_buffer);
    }
}
