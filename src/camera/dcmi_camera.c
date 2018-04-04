#include <ch.h>
#include <hal.h>
#include <stdlib.h>
#include "dcmi_camera.h"

void frameEndCb(DCMIDriver* dcmip);
void dmaTransferEndCb(DCMIDriver* dcmip);
void dmaHalfTransferEndCb(DCMIDriver* dcmip);
void dcmiErrorCb(DCMIDriver* dcmip, dcmierror_t err);

const DCMIConfig dcmicfg = {
    frameEndCb,
    dmaTransferEndCb,
    dmaHalfTransferEndCb,
	dcmiErrorCb,
    DCMI_CR_PCKPOL
};

static capture_mode_t capture_mode = CAPTURE_ONE_SHOT;
static uint8_t *image_buff0 = NULL;
static uint8_t *image_buff1 = NULL;
static uint8_t double_buffering = 0;
static uint8_t image_ready = 0;
static uint8_t dcmiErrorFlag = 0;
static uint8_t dcmi_prepared = 0;
static uint8_t half_transfer_complete = 0;
static uint8_t buff0_busy = 0;
static uint8_t buff1_busy = 0;

//conditional variable
static MUTEX_DECL(dcmi_lock);
static CONDVAR_DECL(dcmi_condvar);

/***************************INTERNAL FUNCTIONS************************************/

// This is called when a complete image is received from the DCMI peripheral.
void frameEndCb(DCMIDriver* dcmip) {
    (void) dcmip;
    //palTogglePad(GPIOD, 13) ; // Orange.
    //signals an image has been captured
    image_ready = 1;
    chSysLockFromISR();
	chCondBroadcastI(&dcmi_condvar);
	chSysUnlockFromISR();
}

// This is called at each DMA transfer completion that correspond to a frame end.
void dmaTransferEndCb(DCMIDriver* dcmip) {
   (void) dcmip;
    //palTogglePad(GPIOD, 15); // Blue.
	//osalEventBroadcastFlagsI(&ss_event, 0);

   half_transfer_complete = 0;
}

void dmaHalfTransferEndCb(DCMIDriver* dcmip) {
	(void) dcmip;

	half_transfer_complete = 1;

	if(double_buffering == 1) {
		if(buff0_busy == 1) {
			if((&DCMID)->dmastp->stream->CR & STM32_DMA_CR_CT) {
				// Mem1 that points to buff1 is being filled and buff0 is being used by the user,
				// thus set Mem0 to point also to buff1 for the next frame in order to
				// leave buff0 usable by the user.
				(&DCMID)->dmastp->stream->M0AR = (uint32_t)image_buff1;
			} else {
				// Mem0 that points to buff1 is being filled and buff0 is being used by the user,
				// thus set Mem1 to point also to buff1 for the next frame in order to
				// leave buff0 usable by the user.
				(&DCMID)->dmastp->stream->M1AR = (uint32_t)image_buff1;
			}
		} else if(buff1_busy == 1) {
			if((&DCMID)->dmastp->stream->CR & STM32_DMA_CR_CT) {
				// Mem1 that points to buff0 is being filled and buff1 is being used by the user,
				// thus set Mem0 to point also to buff0 for the next frame in order to
				// leave buff1 usable by the user.
				(&DCMID)->dmastp->stream->M0AR = (uint32_t)image_buff0;
			} else {
				// Mem0 that points to buff0 is being filled and buff1 is being used by the user,
				// thus set Mem1 to point also to buff0 for the next frame in order to
				// leave buff1 usable by the user.
				(&DCMID)->dmastp->stream->M1AR = (uint32_t)image_buff0;
			}
		} else {
			// Set a different buffer for each memory location.
			if((&DCMID)->dmastp->stream->CR & STM32_DMA_CR_CT) {
				if((uint32_t)image_buff0 == (&DCMID)->dmastp->stream->M1AR) {
					// Mem1 that points to buff0 is being filled and buff1 is free,
					// thus set Mem0 to point to buff1 for the next frame.
					(&DCMID)->dmastp->stream->M0AR = (uint32_t)image_buff1;
				} else {
					// Mem1 that points to buff1 is being filled and buff0 is free,
					// thus set Mem0 to point to buff0 for the next frame.
					(&DCMID)->dmastp->stream->M0AR = (uint32_t)image_buff0;
				}
			} else {
				if((uint32_t)image_buff0 == (&DCMID)->dmastp->stream->M0AR) {
					// Mem0 that points to buff0 is being filled and buff1 is free,
					// thus set Mem1 to point to buff1 for the next frame.
					(&DCMID)->dmastp->stream->M1AR = (uint32_t)image_buff1;
				} else {
					// Mem0 that points to buff1 is being filled and buff0 is free,
					// thus set Mem1 to point to buff0 for the next frame.
					(&DCMID)->dmastp->stream->M1AR = (uint32_t)image_buff0;
				}
			}
		}
	}
}

void dcmiErrorCb(DCMIDriver* dcmip, dcmierror_t err) {
   (void) dcmip;
   (void) err;
    dcmiErrorFlag = 1;
	//chSysHalt("DCMI error");
}

/**
* @brief   Captures a single frame from the DCMI.
* @details This asynchronous function starts a single shot receive operation.
*
* @param[in] dcmip     pointer to the @p DCMIDriver object
*
*/
void dcmi_start_one_shot(DCMIDriver *dcmip) {
	image_ready = 0;
	dcmiStartOneShot(dcmip);
}

/**
* @brief   Begins reception of frames from the DCMI.
* @details This asynchronous function starts a continuous receive operation.
*
* @param[in] dcmip     pointer to the @p DCMIDriver object
*
*/
void dcmi_start_stream(DCMIDriver *dcmip) {
	dcmiStartStream(dcmip);
}

/**
* @brief   Stops reception of frames from the DCMI.
*
* @param[in] dcmip     pointer to the @p DCMIDriver object
*
*
* @return              The operation status.
* @retval MSG_OK       if the function succeeded.
* @retval MSG_TIMEOUT  if a timeout occurred before operation end.
*
*/
msg_t dcmi_stop_stream(DCMIDriver *dcmip) {
	return dcmiStopStream(dcmip);
}

/*************************END INTERNAL FUNCTIONS**********************************/


/****************************PUBLIC FUNCTIONS*************************************/

int8_t dcmi_start(void) {
	dcmiInit();
    if(image_buff0 != NULL) {
        free(image_buff0);
        image_buff0 = NULL;
    }
    image_buff0 = (uint8_t*)malloc(MAX_BUFF_SIZE);
    if(image_buff0 == NULL) {
        return -1;
    }
    return 0;
}

int8_t dcmi_prepare(void) {
	if(dcmi_prepared == 1) {
		dcmiUnprepare(&DCMID);
	}
	// Check if image size fit in the available memory.
	uint32_t image_size = po8030_get_image_size();
	if(double_buffering == 0) {
		if(image_size > MAX_BUFF_SIZE) {
			return -1;
		}
	} else {
		if(image_size > MAX_BUFF_SIZE/2) {
			return -1;
		}
	}
	// Prepare the DCMI and enable the DMA.
	dcmiPrepare(&DCMID, &dcmicfg, image_size, (uint32_t*)image_buff0, (uint32_t*)image_buff1);
	dcmi_prepared = 1;

	return 0;
}

void dcmi_unprepare(void) {
	if(dcmi_prepared == 1) {
		dcmiUnprepare(&DCMID);
	}
	dcmi_prepared = 0;
}

uint8_t image_is_ready(void) {
	return image_ready;
}

void wait_image_ready(void) {
	//waits until an image has been captured
    chMtxLock(&dcmi_lock);
    chCondWait(&dcmi_condvar);
    chMtxUnlock(&dcmi_lock);
}

uint8_t dcmi_double_buffering_enabled(void) {
	return double_buffering;
}

int8_t dcmi_enable_double_buffering(void) {
	double_buffering = 1;

	// Free the first buffer memory that was allocated with the max available memory.
    if(image_buff0 != NULL) {
        free(image_buff0);
        image_buff0 = NULL;
    }
    // Allocate half of the available memory for the first buffer.
    image_buff0 = (uint8_t*)malloc(MAX_BUFF_SIZE/2);
    if(image_buff0 == NULL) {
        return -1;
    }
    // Free the second buffer in case it was already allocated.
    if(image_buff1 != NULL) {
        free(image_buff1);
        image_buff1 = NULL;
    }
    // Allocate half of the available memory for the second buffer.
    image_buff1 = (uint8_t*)malloc(MAX_BUFF_SIZE/2);
    if(image_buff1 == NULL) {
    	return -1;
    }
    return 0;
}

int8_t dcmi_disable_double_buffering(void) {
	double_buffering = 0;

	// Free the second buffer.
    if(image_buff1 != NULL) {
        free(image_buff1);
        image_buff1 = NULL;
    }
	// Free the first buffer.
    if(image_buff0 != NULL) {
        free(image_buff0);
        image_buff0 = NULL;
    }
    // Allocate all of the available memory for the first buffer.
    image_buff0 = (uint8_t*)malloc(MAX_BUFF_SIZE);
    if(image_buff0 == NULL) {
        return -1;
    }
    return 0;
}

void dcmi_set_capture_mode(capture_mode_t mode) {
	capture_mode = mode;
}

uint8_t* dcmi_get_last_image_ptr(void) {
	if(double_buffering == 0) {
		return image_buff0;
	} else {
		if(buff0_busy==1 || buff1_busy==1) { // One buffer at a time can be requested.
			return NULL;
		}
		// mutex on "half_transfer_complete" needed?
		if(half_transfer_complete == 0) {
			if((&DCMID)->dmastp->stream->CR & STM32_DMA_CR_CT) { // Check which buffer is currently being filled.
				//return image_buff0;
				if((uint32_t)image_buff0 == (&DCMID)->dmastp->stream->M0AR) {
					buff0_busy = 1;
				} else {
					buff1_busy = 1;
				}
				return (uint8_t*)((&DCMID)->dmastp->stream->M0AR);
			} else {
				//return image_buff1;
				if((uint32_t)image_buff0 == (&DCMID)->dmastp->stream->M1AR) {
					buff0_busy = 1;
				} else {
					buff1_busy = 1;
				}
				return (uint8_t*)((&DCMID)->dmastp->stream->M1AR);
			}
		} else {
			// One buffer is currently being filled and the other one will be filled at the next "full transfer interrupt" so we cannot give
			// immediately a pointer to the user but we need to wait that the current buffer will be filled completely and return it to the user.
			// If we return the buffer that isn't used at the moment, we cannot be sure it will not still used by the user when the next grabbing
			// start.
			wait_image_ready();
			if((&DCMID)->dmastp->stream->CR & STM32_DMA_CR_CT) { // Check which buffer is currently being filled.
				//return image_buff0;
				if((uint32_t)image_buff0 == (&DCMID)->dmastp->stream->M0AR) {
					buff0_busy = 1;
				} else {
					buff1_busy = 1;
				}
				return (uint8_t*)((&DCMID)->dmastp->stream->M0AR);
			} else {
				//return image_buff1;
				if((uint32_t)image_buff0 == (&DCMID)->dmastp->stream->M1AR) {
					buff0_busy = 1;
				} else {
					buff1_busy = 1;
				}
				return (uint8_t*)((&DCMID)->dmastp->stream->M1AR);
			}
		}
	}
}

void dcmi_release_last_image_ptr(void) {
	buff0_busy = 0;
	buff1_busy = 0;
}

uint8_t* dcmi_get_first_buffer_ptr(void) {
	return image_buff0;
}

uint8_t* dcmi_get_second_buffer_ptr(void) {
	return image_buff1;
}

void dcmi_capture_start(void) {
	if(dcmi_prepared == 0) {
		return;
	}

	buff0_busy = 0;
	buff1_busy = 0;

	if(capture_mode == CAPTURE_ONE_SHOT) {
		dcmi_start_one_shot(&DCMID);
	} else {
		dcmi_start_stream(&DCMID);
	}
}

msg_t dcmi_capture_stop(void) {
	if(dcmi_prepared == 0) {
		return;
	}

	if(capture_mode == CAPTURE_ONE_SHOT) {
		return MSG_OK;
	} else {
		return dcmi_stop_stream(&DCMID);
	}
}

/**************************END PUBLIC FUNCTIONS***********************************/

