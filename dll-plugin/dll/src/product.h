/* product.h: the tool's name and version, in ONE place (modsettings.c and modsettings.rc include it). Plain #defines only: windres reads it. */
#ifndef MS_PRODUCT_H
#define MS_PRODUCT_H
#define MS_PRODUCT      "Mod Settings"      /* as a user reads it (log, status, version resource) */
#define MS_STEM         "modsettings"       /* modsettings.dll / modsettings.ini / modsettings.log / modsettings-status.json */
#define MS_MUTEX        "ModSettings"       /* Local\<this>.<pid>: one copy per game process */
#define MS_VERSION      "0.2.0-tab2"        /* + names / duplicates / clashes, rows in another mod's section, the safety and behaviour rules (TEST) */
#define MS_VER_NUMS     0,2,0,2
#endif
