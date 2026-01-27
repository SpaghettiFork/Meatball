/**
 * Spaghetti Display Server
 * Copyright (C) 2025-2026  SpaghettiFork
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * Sauce.
 * The interposer between Spaghetti and Meatball.
 */

#include <stdarg.h>
#include <xorg/xf86.h>
#include <xorg/xf86Module.h>
#include <xorg/xorg-server.h>
#include <xorg/scrnintstr.h>

#include "meatball/meatball.h"

#define SAUCE_ABI_EXPECTED_MAJOR 1
#define SAUCE_ABI_EXPECTED_MINOR 0

#if defined(SP_GET_RENDER_FD)
#define SAUCE_HAS_RENDER_FD 1
#endif

static MODULESETUPPROTO(sauce_setup);
static MODULETEARDOWNPROTO(sauce_shutdown);

static XF86ModuleVersionInfo VersRec =
{
	.modname      = "sauce",
	.vendor       = "Spaghetti Fork",
	._modinfo1_   = MODINFOSTRING1,
	._modinfo2_   = MODINFOSTRING2,
	.xf86version  = XORG_VERSION_CURRENT,
	.majorversion = 1,
	.minorversion = 0,
	.patchlevel   = 0,
	.abiclass     = ABI_CLASS_EXTENSION,
	.abiversion   = ABI_EXTENSION_VERSION,
};

_X_EXPORT XF86ModuleData sauceModuleData =
{
	.vers = &VersRec,
	.setup = sauce_setup,
	.teardown = sauce_shutdown
};

#if 0
static ScrnInfoPtr
GetPrimaryScreen(void)
{
	for (int i = 0; i < screenInfo.numScreens; i++) {
		const ScreenPtr screen = screenInfo.screens[i];

		if (!screen)
			continue;

		if (!screen->isGPU)
			continue;

		/* Skip any secondary displays. */
		if (screen->current_primary)
			continue;

		return xf86ScreenToScrn(screen);
	}

	return NULL;
}
#endif

static void
sauce_fire(CallbackListPtr *pcbl, void *param, _X_UNUSED void *ext)
{
	struct meatball_config* config = param;
	meatball_initialize(config);

	/* Either it worked or it didn't, stop calling us.*/
	DeleteCallback(pcbl, sauce_fire, param);
}

static void *
sauce_setup(void *module, void *opts, int *errmaj, int *errmin)
{
	static Bool setupDone = FALSE;

	if (setupDone) {
		/* Allow only one instance of Meatball for now. */
		if (errmaj)
		{
			*errmaj = LDR_ONCEONLY;
		}
		return NULL;
	}

#if defined(SAUCE_ABI_EXPECTED_MAJOR) && defined(SAUCE_ABI_EXPECTED_MINOR)
	unsigned long version;
	meatball_version(&version);

	unsigned long major, minor;
	
	major = GET_MODULE_MAJOR_VERSION(version);
	minor = GET_MODULE_MINOR_VERSION(version);

	if (major != SAUCE_ABI_EXPECTED_MAJOR && minor != SAUCE_ABI_EXPECTED_MINOR)
	{
		/* ABI mismatch, likely something would break. */
		if (errmaj)
		{
			*errmaj = LDR_MISMATCH;
		}
		return NULL;
	}
#else
#warning "Sauce: ABI check unavailable, you might be in for a surprise!"
#endif

	setupDone = TRUE;

	struct meatball_config config = { 0 };

	/* Software only for now. */
	config.meatball_flags = MB_FORCE_SOFTWARE_RENDERING;

	/* Because of how modules are loaded in Xorg, we need
	 * to piggyback on a callback to correctly time the 
	 * Meatball initialization call otherwise the server
	 * isn't ready for clients and it will fail to startup. */
	return AddCallback(&EventCallback, sauce_fire, &config) ? module : NULL;
}

static void
sauce_shutdown(void *teardownData)
{
	meatball_shutdown();
}