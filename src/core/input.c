/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - emulate_game_controller_via_input_plugin.c              *
 *   Mupen64Plus homepage: http://code.google.com/p/mupen64plus/           *
 *   Copyright (C) 2014 Bobby Smiles                                       *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.          *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include "core/input.h"
#include "core/backend.h"

#include "core/backend.h"
#include "device/controllers/game_controller.h"
#include <libretro.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define ROUND(x)    floor((x) + 0.5)

/* snprintf not available in MSVC 2010 and earlier */
#include "core/msvc_compat.h"

extern retro_environment_t environ_cb;
extern retro_input_state_t input_cb;
extern struct retro_rumble_interface rumble;
extern int pad_pak_types[4];
extern int pad_present[4];
int astick_deadzone = 15;
 int astick_sensitivity = 100;
static bool libretro_supports_bitmasks = false;

extern core_rom_header ROM_HEADER;

// Some stuff from n-rage plugin
#define RD_GETSTATUS        0x00        // get status
#define RD_READKEYS         0x01        // read button values
#define RD_READPAK          0x02        // read from controllerpack
#define RD_WRITEPAK         0x03        // write to controllerpack
#define RD_RESETCONTROLLER  0xff        // reset controller
#define RD_READEEPROM       0x04        // read eeprom
#define RD_WRITEEPROM       0x05        // write eeprom

#define PAK_IO_RUMBLE       0xC000      // the address where rumble-commands are sent to

/* global data definitions */
struct
{
    core_controller *control;               // pointer to core_controller struct in Core library
    core_buttons buttons;
} controller[4];

void core_input_keys( int Control, core_buttons *Keys );

static void set_input_descriptors(void)
{

      #define independent_cbuttons_map(PAD) \
      { PAD, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "A Button" },\
      { PAD, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "B Button" },\
      { PAD, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      "C-Right" },\
      { PAD, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "C-Left" },\
      { PAD, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "C-Down" },\
      { PAD, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "C-Up" },\
      { PAD, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,     "Z Trigger" },\
      { PAD, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,     "R Shoulder" },\
      { PAD, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "L Shoulder" },\
      { PAD, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Start" },\
      { PAD, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "D-Pad Right" },\
      { PAD, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "D-Pad Left" },\
      { PAD, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "D-Pad Down" },\
      { PAD, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "D-Pad Up" },\
      { PAD, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,  RETRO_DEVICE_ID_ANALOG_X, "Control Stick X" },\
      { PAD, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,  RETRO_DEVICE_ID_ANALOG_Y, "Control Stick Y" },\

      static struct retro_input_descriptor desc[] = {
         independent_cbuttons_map(0)
         independent_cbuttons_map(1)
         independent_cbuttons_map(2)
         independent_cbuttons_map(3)
         0
      };
      environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, (struct retro_input_descriptor*)desc);
}

/* Controller protocol and libretro input translation. */

static unsigned char DataCRC( unsigned char *Data, int iLenght )
{
    unsigned char Remainder = Data[0];

    int iByte = 1;
    unsigned char bBit = 0;

    while( iByte <= iLenght )
    {
        int HighBit = ((Remainder & 0x80) != 0);
        Remainder = Remainder << 1;

        Remainder += ( iByte < iLenght && Data[iByte] & (0x80 >> bBit )) ? 1 : 0;

        Remainder ^= (HighBit) ? 0x85 : 0;

        bBit++;
        iByte += bBit/8;
        bBit %= 8;
    }

    return Remainder;
}

/******************************************************************
  Function: ControllerCommand
  Purpose:  To process the raw data that has just been sent to a
            specific controller.
  input:    - Controller Number (0 to 3) and -1 signalling end of
              processing the pif ram.
            - Pointer of data to be processed.
  output:   none

  note:     This function is only needed if the DLL is allowing raw
            data, or the plugin is set to raw

            the data that is being processed looks like this:
            initilize controller: 01 03 00 FF FF FF
            read controller:      01 04 01 FF FF FF FF
*******************************************************************/
void core_input_command(int Control, unsigned char *Command)
{
    unsigned char *Data = &Command[5];

    if (Control == -1)
        return;

    switch (Command[2])
    {
        case RD_GETSTATUS:
            break;
        case RD_READKEYS:
            break;
        case RD_READPAK:
            if (controller[Control].control->accessory == ACCESSORY_RAW)
            {
                unsigned int dwAddress = (Command[3] << 8) + (Command[4] & 0xE0);

                if(( dwAddress >= 0x8000 ) && ( dwAddress < 0x9000 ) )
                    memset( Data, 0x80, 32 );
                else
                    memset( Data, 0x00, 32 );

                Data[32] = DataCRC( Data, 32 );
            }
            break;
        case RD_WRITEPAK:
            if (controller[Control].control->accessory == ACCESSORY_RAW)
            {
                unsigned int dwAddress = (Command[3] << 8) + (Command[4] & 0xE0);
                Data[32] = DataCRC( Data, 32 );

                if ((dwAddress == PAK_IO_RUMBLE) && (rumble.set_rumble_state))
                {
                    if (*Data)
                    {
                        rumble.set_rumble_state(Control, RETRO_RUMBLE_WEAK, 0xFFFF);
                        rumble.set_rumble_state(Control, RETRO_RUMBLE_STRONG, 0xFFFF);
                    }
                    else
                    {
                        rumble.set_rumble_state(Control, RETRO_RUMBLE_WEAK, 0);
                        rumble.set_rumble_state(Control, RETRO_RUMBLE_STRONG, 0);
                    }
                }
            }

            break;
        case RD_RESETCONTROLLER:
            break;
        case RD_READEEPROM:
            break;
        case RD_WRITEEPROM:
            break;
        }
}

/******************************************************************
  Function: GetKeys
  Purpose:  To get the current state of the controllers buttons.
  input:    - Controller Number (0 to 3)
            - A pointer to a core_buttons structure to be filled with
            the controller state.
  output:   none
*******************************************************************/

// System analog stick range is -0x8000 to 0x8000
#define ASTICK_MAX 0x8000
#define CSTICK_DEADZONE 0x4000

#define CSTICK_RIGHT 0x200
#define CSTICK_LEFT 0x100
#define CSTICK_UP 0x800
#define CSTICK_DOWN 0x400


static void translate_analog_axes(int16_t analogX, int16_t analogY, int Control, core_buttons* Keys)
{
   double radius, angle;
   //  Keys->Value |= input_cb(Control, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_XX)    ? 0x4000 : 0; // Mempak switch
   //  Keys->Value |= input_cb(Control, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_XX)    ? 0x8000 : 0; // Rumblepak switch

   analogX = input_cb(Control, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);
   analogY = input_cb(Control, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y);

   // Convert cartesian coordinate analog stick to polar coordinates
   radius = sqrt(analogX * analogX + analogY * analogY);
   angle = atan2(analogY, analogX);

   if (radius > astick_deadzone)
   {
      // Re-scale analog stick range to negate deadzone (makes slow movements possible)
      radius = (radius - astick_deadzone)*((float)ASTICK_MAX/(ASTICK_MAX - astick_deadzone));
      // N64 Analog stick range is from -80 to 80
      radius *= 80.0 / ASTICK_MAX * (astick_sensitivity / 100.0);
      // Convert back to cartesian coordinates
      Keys->X_AXIS = +(int32_t)ROUND(radius * cos(angle));
      Keys->Y_AXIS = -(int32_t)ROUND(radius * sin(angle));
   }
   else
   {
      Keys->X_AXIS = 0;
      Keys->Y_AXIS = 0;
   }
}

void core_input_keys( int Control, core_buttons *Keys )
{
   unsigned i;
   bool cbuttons_mode = false;
   int16_t ret        = 0;
   int16_t analogX    = 0;
   int16_t analogY    = 0;
   Keys->Value        = 0;

   if (libretro_supports_bitmasks)
      ret = input_cb(Control, RETRO_DEVICE_JOYPAD,
            0, RETRO_DEVICE_ID_JOYPAD_MASK);
   else
   {
      for (i = 0; i <= RETRO_DEVICE_ID_JOYPAD_R3; i++)
      {
         if (input_cb(Control, RETRO_DEVICE_JOYPAD, 0, i))
            ret |= (1 << i);
      }
   }

   Keys->R_DPAD       = !!((ret & (1 << RETRO_DEVICE_ID_JOYPAD_RIGHT)));
   Keys->L_DPAD       = !!((ret & (1 << RETRO_DEVICE_ID_JOYPAD_LEFT)));
   Keys->D_DPAD       = !!((ret & (1 << RETRO_DEVICE_ID_JOYPAD_DOWN)));
   Keys->U_DPAD       = !!((ret & (1 << RETRO_DEVICE_ID_JOYPAD_UP)));
   Keys->START_BUTTON = !!((ret & (1 << RETRO_DEVICE_ID_JOYPAD_START)));
   Keys->Z_TRIG       = !!((ret & (1 << RETRO_DEVICE_ID_JOYPAD_L2)));

      Keys->A_BUTTON  = !!((ret & (1 << RETRO_DEVICE_ID_JOYPAD_B)));
      Keys->B_BUTTON  = !!((ret & (1 << RETRO_DEVICE_ID_JOYPAD_Y)));
      Keys->R_CBUTTON = !!((ret & (1 << RETRO_DEVICE_ID_JOYPAD_R)));
      Keys->L_CBUTTON = !!((ret & (1 << RETRO_DEVICE_ID_JOYPAD_L)));
      Keys->D_CBUTTON = !!((ret & (1 << RETRO_DEVICE_ID_JOYPAD_A)));
      Keys->U_CBUTTON = !!((ret & (1 << RETRO_DEVICE_ID_JOYPAD_X)));
      Keys->R_TRIG    = !!((ret & (1 << RETRO_DEVICE_ID_JOYPAD_R2)));
   Keys->L_TRIG    = !!((ret & (1 <<RETRO_DEVICE_ID_JOYPAD_SELECT)));

   translate_analog_axes(analogX, analogY, Control, Keys);
}

void core_input_init(void)
{
    int i;
    libretro_supports_bitmasks = environ_cb(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, NULL);
    for( i = 0; i < 4; i++ )
    {
       controller[i].control = Controls + i;
       controller[i].control->Present = pad_present[i];
       controller[i].control->RawData = 0;

       if (pad_pak_types[i] == ACCESSORY_MEMPAK)
          controller[i].control->accessory = ACCESSORY_MEMPAK;
       else if (pad_pak_types[i] == ACCESSORY_RAW)
          controller[i].control->accessory = ACCESSORY_RAW;
       else if (pad_pak_types[i] == ACCESSORY_TRANSFER_PAK)
          controller[i].control->accessory = ACCESSORY_TRANSFER_PAK;
       else
          controller[i].control->accessory = ACCESSORY_NONE;
    }
   set_input_descriptors();
}


void core_input_read(int Control, unsigned char *Command)
{
   core_input_command(Control, Command);
}

