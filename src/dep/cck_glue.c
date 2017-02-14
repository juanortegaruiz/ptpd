/*-
 * Copyright (c) 2016      Wojciech Owczarek
 *
 * All Rights Reserved
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * @file   cck_glue.c
 * @date   Thu Nov 24 23:19:00 2016
 *
 * @brief  Wrappers configuring LibCCK components from GlobalConfig *
 *
 *
 */

#include <string.h>
#include <math.h>

#include "cck_glue.h"

#include <libcck/cck_utils.h>
#include <libcck/transport_address.h>
#include <libcck/clockdriver.h>
#include <libcck/ttransport.h>
#include <libcck/timer.h>

#include "../ptp_timers.h"

#define CREATE_ADDR(var) \
	var = createCckTransportAddress();

#define FREE_ADDR(var) \
	freeCckTransportAddress((CckTransportAddress **)(&(var)))

static bool pushClockDriverPrivateConfig_unix(ClockDriver *driver, const GlobalConfig *global);

#ifdef CCK_BUILD_CLOCKDRIVER_LINUXPHC
static bool pushClockDriverPrivateConfig_linuxphc(ClockDriver *driver, const GlobalConfig *global);
#endif

static void ptpTimerExpiry(void *self, void *owner);
static int setupPtpTimer(PtpTimer *timer, CckFdSet *fdSet, const char *name);

bool
configureClockDriver(ClockDriver *driver, const void *configData)
{

    const GlobalConfig *global = configData;

    bool ret = TRUE;

    ClockDriverConfig *config = &driver->config;

    config->stepType = CSTEP_ALWAYS;

    if(global->noResetClock) {
	config->stepType = CSTEP_NEVER;
    }

    if(global->stepOnce) {
	config->stepType = CSTEP_STARTUP;
    }

    if(global->stepForce) {
	config->stepType = CSTEP_STARTUP_FORCE;
    }

    config->disabled = (tokenInList(global->disabledClocks, driver->name, DEFAULT_TOKEN_DELIM)) ? TRUE:FALSE;
    config->excluded = (tokenInList(global->excludedClocks, driver->name, DEFAULT_TOKEN_DELIM)) ? TRUE:FALSE;

    if(global->noAdjust) {
	config->readOnly = TRUE;
    } else {
	config->readOnly = (tokenInList(global->readOnlyClocks, driver->name, DEFAULT_TOKEN_DELIM)) ? TRUE:FALSE;
    }

    if(config->required && config->disabled) {
	if(!config->readOnly) {
	    WARNING("clock: clock %s cannot be disabled - set to read-only to exclude from sync\n", driver->name);
	} else {
	    WARNING("clock: clock %s cannot be disabled - already set to read-only\n", driver->name);
	}
	config->disabled = FALSE;
    }

    config->noStep = global->noResetClock;
    config->negativeStep = global->negativeStep;
    config->storeToFile = global->storeToFile;
    config->adevPeriod = global->adevPeriod;
    config->stableAdev = global->stableAdev;
    config->unstableAdev = global->unstableAdev;
    config->lockedAge = global->lockedAge;
    config->holdoverAge = global->holdoverAge;
    config->stepTimeout = (global->enablePanicMode) ? global-> panicModeDuration : 0;
    config->calibrationTime = global->clockCalibrationTime;
    config->stepExitThreshold = global->panicModeExitThreshold;
    config->strictSync = global->clockStrictSync;
    config->minStep = global->clockMinStep;

    config->statFilter = global->clockStatFilterEnable;
    config->filterWindowSize = global->clockStatFilterWindowSize;

    if(config->filterWindowSize == 0) {
	config->filterWindowSize = global->clockSyncRate;
    }

    config->filterType = global->clockStatFilterType;
    config->filterWindowType = global->clockStatFilterWindowType;

    config->outlierFilter = global->clockOutlierFilterEnable;
    config->madWindowSize = global->clockOutlierFilterWindowSize;
    config->madDelay = global->clockOutlierFilterDelay;
    config->madMax = global->clockOutlierFilterCutoff;
    config->outlierFilterBlockTimeout = global->clockOutlierFilterBlockTimeout;

    driver->servo.kP = global->servoKP;
    driver->servo.kI = global->servoKI;

    driver->servo.maxOutput = global->servoMaxPpb;
    driver->servo.tauMethod = global->servoDtMethod;
    driver->servo.maxTau = global->servoMaxdT;

    strncpy(config->frequencyDir, global->frequencyDir, PATH_MAX);

    configureClockDriverFilters(driver);

    #define CCK_REGISTER_IMPL(drvtype, suffix, strname) \
	if(driver->type == drvtype) { \
	    ret = ret && (pushClockDriverPrivateConfig_##suffix(driver, global)); \
	}

    #include <libcck/clockdriver.def>

    DBG("clock: Clock driver %s configured\n", driver->name);

    return ret;

}

#ifdef CCK_BUILD_CLOCKDRIVER_LINUXPHC
static bool
pushClockDriverPrivateConfig_linuxphc(ClockDriver *driver, const GlobalConfig *global)
{

    ClockDriverConfig *config = &driver->config;

    CCK_GET_PCONFIG(ClockDriver, linuxphc, driver, myConfig);

    config->stableAdev = global->stableAdev_hw;
    config->unstableAdev = global->unstableAdev_hw;
    config->lockedAge = global->lockedAge_hw;
    config->holdoverAge = global->holdoverAge_hw;
    config->negativeStep = global->negativeStep_hw;

    myConfig->lockDevice = global->lockClockDevice;

    driver->servo.kP = global->servoKP_hw;
    driver->servo.kI = global->servoKI_hw;
    driver->servo.maxOutput = global->servoMaxPpb_hw;

    return TRUE;

}
#endif

static bool
pushClockDriverPrivateConfig_unix(ClockDriver *driver, const GlobalConfig *global)
{

    CCK_GET_PCONFIG(ClockDriver, unix, driver, myConfig);

    myConfig->setRtc = global->setRtc;

    return TRUE;

}

void
ptpPortPostInit(PtpClock *ptpClock)
{

    DBG("ptpPortInit() called\n");

    for(int i = 0; i < UNICAST_MAX_DESTINATIONS; i++) {
	CREATE_ADDR(ptpClock->syncDestIndex[i].protocolAddress);
	CREATE_ADDR(ptpClock->unicastDestinations[i].protocolAddress);
	CREATE_ADDR(ptpClock->unicastGrants[i].protocolAddress);
    }

    for(int j = 0; j < ptpClock->global->fmrCapacity; j++) {
	CREATE_ADDR(ptpClock->foreign[j].protocolAddress);
    }

    CREATE_ADDR(ptpClock->generalDestination);
    CREATE_ADDR(ptpClock->eventDestination);

    CREATE_ADDR(ptpClock->peerGeneralDestination);
    CREATE_ADDR(ptpClock->peerEventDestination);

    CREATE_ADDR(ptpClock->unicastPeerDestination.protocolAddress);
    CREATE_ADDR(ptpClock->lastSyncDst);
    CREATE_ADDR(ptpClock->lastPdelayRespDst);

    /* set up callbacks */
    ptpClock->callbacks.onStateChange = ptpPortStateChange;

}

void
ptpPortPreShutdown(PtpClock *ptpClock)
{

    DBG("ptpPortShutdown() called\n");

    for(int i = 0; i < UNICAST_MAX_DESTINATIONS; i++) {
	FREE_ADDR(ptpClock->syncDestIndex[i].protocolAddress);
	FREE_ADDR(ptpClock->unicastDestinations[i].protocolAddress);
	FREE_ADDR(ptpClock->unicastGrants[i].protocolAddress);
    }

    for(int j = 0; j < ptpClock->fmrCapacity; j++) {
	FREE_ADDR(ptpClock->foreign[j].protocolAddress);
    }

    FREE_ADDR(ptpClock->generalDestination);
    FREE_ADDR(ptpClock->eventDestination);

    FREE_ADDR(ptpClock->peerGeneralDestination);
    FREE_ADDR(ptpClock->peerEventDestination);

    FREE_ADDR(ptpClock->unicastPeerDestination.protocolAddress);
    FREE_ADDR(ptpClock->lastSyncDst);
    FREE_ADDR(ptpClock->lastPdelayRespDst);

}

void
ptpPortStepNotify(void *owner)
{

    PtpClock *ptpClock = (PtpClock*)owner;

    DBG("PTP clock was notified of clock step - resetting outlier filters and re-calibrating offsets\n");

    recalibrateClock(ptpClock, true);
}

void
ptpPortUpdate(void *owner)
{

    PtpClock *ptpClock = owner;

    if(ptpClock && ptpClock->masterClock) {

	ClockDriver *mc = ptpClock->masterClock;

	if((ptpClock->portDS.portState == PTP_MASTER) || (ptpClock->portDS.portState == PTP_PASSIVE)) {
	    mc->setState(mc, CS_LOCKED);
	}

	mc->touchClock(mc);

    }

}

void
ptpPortLocked(void *owner, bool locked)
{

    PtpClock *ptpClock = owner;

    if(ptpClock) {
	ptpClock->clockLocked = locked;
    }

}

void
ptpPortStateChange(PtpClock *ptpClock, const uint8_t from, const uint8_t to)
{

    ClockDriver *cd = ptpClock->clockDriver;
    ClockDriver *mc = ptpClock->masterClock;


    if(!cd) {
	return;
    }

    if(mc) {
	/* entering MASTER or PASSIVE */
	if((to == PTP_MASTER) || (to == PTP_PASSIVE)) {
	    mc->setExternalReference(mc, "PREFMST", RC_EXTERNAL);
	/* leaving MASTER or PASSIVE */
	} else if((from == PTP_MASTER) || (from == PTP_PASSIVE)) {
	    mc->setReference(mc, NULL);
	}
    }

    if(to == PTP_SLAVE) {
	    cd->setExternalReference(cd, "PTP", RC_PTP);
	    cd->owner = ptpClock;
	    cd->callbacks.onStep = ptpPortStepNotify;
	    cd->callbacks.onLock = ptpPortLocked;
    } else if (from == PTP_SLAVE) {
	    if(!mc || (mc != cd)) {
		cd->setReference(cd, NULL);
	    }
	    cd->callbacks.onStep = NULL;
	    cd->callbacks.onLock = NULL;
    }

}

static void
ptpTimerExpiry(void *self, void *owner)
{

    PtpTimer *timer = owner;

    timer->expired = true;

    DBG("Timer %s expired\n", timer->name);

}

static int
setupPtpTimer(PtpTimer *timer, CckFdSet *fdSet, const char *name)
{

    CckTimer *myTimer = NULL;

    memset(timer, 0, sizeof(PtpTimer));
    strncpy(timer->name, name, PTP_TIMER_NAME_MAX);

    myTimer = createCckTimer(CCKTIMER_ANY, name);

    if(!myTimer) {
	return 0;
    }

    if(myTimer->init(myTimer, false, fdSet) != 1) {
	freeCckTimer(&myTimer);
	return 0;
    }

    myTimer->owner = timer;
    myTimer->callbacks.onExpired = ptpTimerExpiry;
    timer->data = myTimer;

    return 1;

}

bool
setupPtpTimers(PtpClock *ptpClock, CckFdSet *fdSet)
{

    for(int i=0; i < PTP_MAX_TIMER; i++) {
	    if(setupPtpTimer(&ptpClock->timers[i], fdSet, getPtpTimerName(i)) != 1) {
		return false;
	    }
    }

    return true;

}

void
shutdownPtpTimers(PtpClock *ptpClock)
{

    for(int i=0; i<PTP_MAX_TIMER; i++) {
	if(ptpClock->timers[i].data) {
	freeCckTimer((CckTimer**)&ptpClock->timers[i].data);
	    memset(&ptpClock->timers[i], 0, sizeof(PtpTimer));
	}
    }

}

bool
prepareClockDrivers(PtpClock *ptpClock, const GlobalConfig *global) {


	TTransport *ev = ptpClock->eventTransport;
	ClockDriver *old = ptpClock->clockDriver;

	/*
	 * if this driver is still there, getClockDriver() should mark it
	 * as required again. If not (say, bond member removed),
	 * it will be cleaned up on next cleanup.
	 */
	if(old) {
	    old->config.required = false;
	    old->inUse = false;
	}

	ClockDriver *cd = ev->getClockDriver(ev);

	/* we are pretty much useless without a working clock driver. */
	if(!cd) {
	    return false;
	}

	configureClockDriver(getSystemClock(), global);

	/* clock driver change */
	if(old &&  (cd != old)) {
	    DBG("PTP port clock driver changing from '%s' to '%s\n",
		    old->name, cd->name);
	    old->setReference(old, NULL);
	    old->owner = NULL;
	    memset(&old->callbacks, 0, sizeof(old->callbacks));
	}

	ptpClock->clockDriver = cd;

	configureClockDriver(cd, global);
	cd->inUse = true;
	cd->config.required = true;

	/* if we are slave, set the reference straigh away */
	if(ptpClock->portDS.portState == PTP_SLAVE) {
	    cd->setExternalReference(ptpClock->clockDriver, "PTP", RC_PTP);
	    cd->owner = ptpClock;
	    /* clean callbacks, we could have had them from somewhere else */
	    memset(&cd->callbacks, 0, sizeof(cd->callbacks));
	    cd->callbacks.onStep = ptpPortStepNotify;
	    cd->callbacks.onLock = ptpPortLocked;
	}

	ClockDriver *lastMaster = ptpClock->masterClock;

	if(strlen(global->masterClock) > 0) {
	    ptpClock->masterClock = getClockDriverByName(global->masterClock);
	    if(ptpClock->masterClock == NULL) {
		WARNING("Could not find designated master clock: %s\n", global->masterClock);
	    }
	} else {
	    ptpClock->masterClock = NULL;
	}

	if((lastMaster) && (lastMaster != ptpClock->masterClock)) {
	    lastMaster->setReference(lastMaster, NULL);
	    lastMaster->setState(lastMaster, CS_FREERUN);
	}

	if((ptpClock->masterClock) && (ptpClock->masterClock != ptpClock->clockDriver)) {
	    if( (ptpClock->portDS.portState == PTP_MASTER) || (ptpClock->portDS.portState == PTP_PASSIVE)) {
		((ClockDriver*)ptpClock->masterClock)->setExternalReference(ptpClock->masterClock, "PREFMST", RC_EXTERNAL);
	    }
	}

	cd->callbacks.onUpdate = ptpPortUpdate;

	return true;


}
