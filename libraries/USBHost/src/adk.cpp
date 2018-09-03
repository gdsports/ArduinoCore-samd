/* Copyright (C) 2011 Circuits At Home, LTD. All rights reserved.

This software may be distributed and modified under the terms of the GNU
General Public License version 2 (GPL2) as published by the Free Software
Foundation and appearing in the file GPL2.TXT included in the packaging of
this file. Please note that GPL2 Section 2[b] requires that all works based
on this software must also be made publicly available under the terms of
the GPL2 ("Copyleft").

Contact information
-------------------

Circuits At Home, LTD
Web      :  http://www.circuitsathome.com
e-mail   :  support@circuitsathome.com
*/

/* Google ADK interface */

#include "adk.h"

const uint32_t ADK::epDataInIndex  = 1;
const uint32_t ADK::epDataOutIndex = 2;

ADK::ADK(USBHost *p, const char* manufacturer,
        const char* model,
        const char* description,
        const char* version,
        const char* uri,
        const char* serial) :

/* ADK ID Strings */
manufacturer(manufacturer),
model(model),
description(description),
version(version),
uri(uri),
serial(serial),
pUsb(p), //pointer to USB class instance - mandatory
bAddress(0), //device address - mandatory
bConfNum(0), //configuration number
bNumEP(1), //if config descriptor needs to be parsed
ready(false) {
        // initialize endpoint data structures
	for (uint32_t i = 0; i < ADK_MAX_ENDPOINTS; i++) {
		epInfo[i].epAddr	= 0;
		epInfo[i].maxPktSize	= (i) ? 0 : 8;
		epInfo[i].bmAttribs	= 0;
		epInfo[i].bmSndToggle   = 0;
		epInfo[i].bmRcvToggle   = 0;
		epInfo[i].bmNakPower  	= (i) ? USB_NAK_NOWAIT : USB_NAK_MAX_POWER;
        }//for(uint32_t i=0; i<ADK_MAX_ENDPOINTS; i++...

        // register in USB subsystem
        if(pUsb) {
                pUsb->RegisterDeviceClass(this); //set devConfig[] entry
        }
}

uint32_t ADK::ConfigureDevice(uint32_t parent, uint32_t port, uint32_t lowspeed) {
        return Init(parent, port, lowspeed); // Just call Init. Yes, really!
}

/* Connection initialization of an Android phone */
uint32_t ADK::Init(uint32_t parent, uint32_t port, uint32_t lowspeed) {
        uint8_t buf[sizeof (USB_DEVICE_DESCRIPTOR)];
        USB_DEVICE_DESCRIPTOR * udd = reinterpret_cast<USB_DEVICE_DESCRIPTOR*>(buf);
        uint32_t rcode;
        uint32_t num_of_conf; // number of configurations
        UsbDeviceDefinition *p = NULL;
        EpInfo *oldep_ptr = NULL;

        // get memory address of USB device address pool
        AddressPool &addrPool = pUsb->GetAddressPool();

        USBTRACE("\r\nADK Init");

        // check if address has already been assigned to an instance
        if(bAddress) {
                USBTRACE("\r\nAddress in use");
                return USB_ERROR_CLASS_INSTANCE_ALREADY_IN_USE;
        }

        // Get pointer to pseudo device with address 0 assigned
        p = addrPool.GetUsbDevicePtr(0);

        if(!p) {
                USBTRACE("\r\nAddress not found");
                return USB_ERROR_ADDRESS_NOT_FOUND_IN_POOL;
        }

        if(!p->epinfo) {
                USBTRACE("epinfo is null\r\n");
                return USB_ERROR_EPINFO_IS_NULL;
        }

	// Save old pointer to EP_RECORD of address 0
	oldep_ptr = p->epinfo;

	// Temporary assign new pointer to epInfo to p->epinfo in order to avoid toggle inconsistence
	p->epinfo = epInfo;

	p->lowspeed = lowspeed;

	// Get device descriptor  GET_DESCRIPTOR
	rcode = pUsb->getDevDescr(0, 0, sizeof(USB_DEVICE_DESCRIPTOR), (uint8_t*)buf);

	// Restore p->epinfo
	p->epinfo = oldep_ptr;

	if(rcode) {
        USBTRACE("\r\nGetDevDesc1 Error ");
		goto FailGetDevDescr;
	}

	// Reset
	UHD_BusReset();
	while( Is_uhd_starting_reset() ) {}

	// Allocate new address according to device class
	bAddress = addrPool.AllocAddress(parent, false, port);
	if (!bAddress)
		return USB_ERROR_OUT_OF_ADDRESS_SPACE_IN_POOL;


	// Extract Max Packet Size from device descriptor
	epInfo[0].maxPktSize = udd->bMaxPacketSize0;

	// Assign new address to the device  SET_ADDRESS
	rcode = pUsb->setAddr(0, 0, bAddress);
	if(rcode) {
        USBTRACE("\r\nsetAddr Error ");
		p->lowspeed = false;
		addrPool.FreeAddress(bAddress);
		bAddress = 0;
		TRACE_USBHOST(printf("ADK::Init : setAddr failed with rcode %lu\r\n", rcode);)
		return rcode;
    }//if (rcode...

    USBTRACE2("\r\nAddr:", bAddress);
    // Spec says you should wait at least 200ms.
    //delay(300);

 	// Save old pointer to EP_RECORD of address 0
	oldep_ptr = p->epinfo;
	// Temporary assign new pointer to epInfo to p->epinfo in order to avoid toggle inconsistence
	p->epinfo = epInfo;
	// Get device descriptor  GET_DESCRIPTOR
	rcode = pUsb->getDevDescr(bAddress, 0, sizeof(USB_DEVICE_DESCRIPTOR), (uint8_t*)buf);
	// Restore p->epinfo
	p->epinfo = oldep_ptr;
	if (rcode)
	{
        USBTRACE("\r\nGetDevDesc2 Error ");
		goto FailGetDevDescr;
	}

	TRACE_USBHOST(printf("ADK::Init : device address is now %lu\r\n", bAddress);)

	p->lowspeed = false;

	//get pointer to assigned address record
	p = addrPool.GetUsbDevicePtr(bAddress);
	if(!p) {
		return USB_ERROR_ADDRESS_NOT_FOUND_IN_POOL;
	}

	p->lowspeed = lowspeed;

	// Assign epInfo to epinfo pointer - only EP0 is known
	rcode = pUsb->setEpInfoEntry(bAddress, 1, epInfo);
	if(rcode) {
		goto FailSetDevTblEntry;
	}

	//check if ADK device is already in accessory mode; if yes, configure and exit
	if(udd->idVendor == ADK_VID &&
			(udd->idProduct == ADK_PID || udd->idProduct == ADB_PID)) {
		USBTRACE("\r\nAcc.mode device detected");
		/* go through configurations, find first bulk-IN, bulk-OUT EP, fill epInfo and quit */
		num_of_conf = udd->bNumConfigurations;

		USBTRACE2("\r\nNC:",num_of_conf);
		for (uint32_t i = 0; i < num_of_conf; i++) {
			ConfigDescParser<0, 0, 0, 0> confDescrParser(this);

			delay(1);
			rcode = pUsb->getConfDescr(bAddress, 0, i, &confDescrParser);

#if defined(XOOM)
			//added by Jaylen Scott Vanorden
			if(rcode) {
				TRACE_USBHOST(printf("ADK::Init : Got 1st bad code for config: %lu\r\n", rcode);)

				// Try once more
				rcode = pUsb->getConfDescr(bAddress, 0, i, &confDescrParser);
			}
#endif
			if(rcode) {
				goto FailGetConfDescr;
			}
			if(bNumEP > 2) {
				break;
			}
		} // for (uint32_t i=0; i<num_of_conf; i++...

 		if(bNumEP == 3) {
			// Assign epInfo to epinfo pointer - this time all 3 endpoins
			rcode = pUsb->setEpInfoEntry(bAddress, 3, epInfo);
			if(rcode) {
				goto FailSetDevTblEntry;
			}
		}

		// Set Configuration Value
		rcode = pUsb->setConf(bAddress, 0, bConfNum);
		if(rcode) {
			goto FailSetConfDescr;
		}
		/* print endpoint structure */
		
		USBTRACE("\r\nEndpoint Structure:");
		USBTRACE("\r\nEP0:");
		USBTRACE2("\r\nAddr: ", epInfo[0].epAddr);
		USBTRACE2("\r\nMax.pkt.size: ", epInfo[0].maxPktSize);
		USBTRACE2("\r\nAttr: ", epInfo[0].epAttribs);
		USBTRACE("\r\nEpout:");
		USBTRACE2("\r\nAddr: ", epInfo[epDataOutIndex].epAddr);
		USBTRACE2("\r\nMax.pkt.size: ", epInfo[epDataOutIndex].maxPktSize);
		USBTRACE2("\r\nAttr: ", epInfo[epDataOutIndex].epAttribs);
		USBTRACE("\r\nEpin:");
		USBTRACE2("\r\nAddr: ", epInfo[epDataInIndex].epAddr);
		USBTRACE2("\r\nMax.pkt.size: ", epInfo[epDataInIndex].maxPktSize);
		USBTRACE2("\r\nAttr: ", epInfo[epDataInIndex].epAttribs);
	

		USBTRACE("\r\nConfiguration successful");
		ready = true;
		return 0; //successful configuration
    }//if( buf->idVendor == ADK_VID...

	//probe device - get accessory protocol revision
	{
        uint32_t adkproto = 0;
        delay(1);
        //rcode = getProto((uint8_t*) & adkproto);  // ADK_GETPROTO  0x33
		rcode = getProto((uint8_t*)buf);
				
#if defined(XOOM)
        //added by Jaylen Scott Vanorden
        if(rcode) {
            USBTRACE2("\r\nGot 1st bad code for proto: ", rcode);
            // Try once more
            rcode = getProto((uint8_t*) & adkproto);
        }
#endif
        if(rcode) {
            goto FailGetProto; //init fails
        }
		adkproto = buf[0] | (buf[1]<<8);
        USBTRACE2("\r\nADK protocol rev. ", adkproto);
    }

    delay(100);

    //sending ID strings
	// ADK_SENDSTR 0x34
    if( sendStr(ACCESSORY_STRING_MANUFACTURER, manufacturer) ) {
		USBTRACE("\r\nPb ACCESSORY_STRING_MANUFACTURER"); }
    delay(10);
    if( sendStr(ACCESSORY_STRING_MODEL, model) ) {
		USBTRACE("\r\nPb ACCESSORY_STRING_MODEL"); }
    delay(10);
    if( sendStr(ACCESSORY_STRING_DESCRIPTION, description) ) {
		USBTRACE("\r\nPb ACCESSORY_STRING_DESCRIPTION"); }
    delay(10);
    if( sendStr(ACCESSORY_STRING_VERSION, version) ) {
		USBTRACE("\r\nPb ACCESSORY_STRING_VERSION"); }
    delay(10);
    if( sendStr(ACCESSORY_STRING_URI, uri) ) {
		USBTRACE("\r\nPb ACCESSORY_STRING_URI"); }
    delay(10);
    if( sendStr(ACCESSORY_STRING_SERIAL, serial) ) {
		USBTRACE("\r\nPb ACCESSORY_STRING_SERIAL"); }

    delay(100);

    USBTRACE("\r\nSwitch to accessory mode");
    //switch to accessory mode
    //the Android phone will reset
    rcode = switchAcc();
    if(rcode) {
            goto FailSwAcc; //init fails
    }
    rcode = USB_ERROR_CONFIG_REQUIRES_ADDITIONAL_RESET;
    delay(100); // Give Android a chance to do its reset. This is a guess, and possibly could be lower.
    goto SwAttempt; //switch to accessory mode attempted

    /* diagnostic messages */
FailGetDevDescr:
#ifdef DEBUG_USB_HOST
	NotifyFailGetDevDescr(rcode);
	goto Fail;
#endif

FailSetDevTblEntry:
#ifdef DEBUG_USB_HOST
	NotifyFailSetDevTblEntry(rcode);
	goto Fail;
#endif

FailGetConfDescr:
#ifdef DEBUG_USB_HOST
    NotifyFailGetConfDescr(rcode);
    goto Fail;
#endif

FailSetConfDescr:
#ifdef DEBUG_USB_HOST
    NotifyFailSetConfDescr(rcode);
    goto Fail;
#endif

FailGetProto:
#ifdef DEBUG_USB_HOST
    USBTRACE("\r\ngetProto:");
    goto Fail;
#endif

FailSwAcc:
#ifdef DEBUG_USB_HOST
    USBTRACE("\r\nswAcc:");
    goto Fail;
#endif

    //FailOnInit:
    //	USBTRACE("OnInit:");
    //	goto Fail;
    //
SwAttempt:
#ifdef DEBUG_USB_HOST
    USBTRACE("\r\nAccessory mode switch attempt");
Fail:
#endif
	USBTRACE2("\r\nADK Init Failed, error code: ", rcode);
	//NotifyFail(rcode);
	Release();
	return rcode;
}

/* Extracts bulk-IN and bulk-OUT endpoint information from config descriptor */
void ADK::EndpointXtract(uint32_t conf, uint32_t /* iface */, uint32_t /* alt */, uint32_t /* proto */, const USB_ENDPOINT_DESCRIPTOR *pep) {
        //ErrorMessage<uint8_t>(PSTR("Conf.Val"), conf);
        //ErrorMessage<uint8_t>(PSTR("Iface Num"), iface);
        //ErrorMessage<uint8_t>(PSTR("Alt.Set"), alt);

        //added by Yuuichi Akagawa
        if(bNumEP == 3) {
		return;
	}

	bConfNum = conf;

        if((pep->bmAttributes & 0x02) == 2) {
                uint32_t index = ((pep->bEndpointAddress & 0x80) == 0x80) ? epDataInIndex : epDataOutIndex;
                // Fill in the endpoint info structure
                epInfo[index].epAddr = (pep->bEndpointAddress & 0x0F);
                epInfo[index].maxPktSize = (uint8_t)pep->wMaxPacketSize;
                epInfo[index].bmAttribs = pep->bmAttributes;
                epInfo[index].bmSndToggle = 0;
                epInfo[index].bmRcvToggle = 0;

                bNumEP++;

                //PrintEndpointDescriptor(pep);
        }
}

/* Performs a cleanup after failed Init() attempt */
uint32_t ADK::Release() {
	pUsb->GetAddressPool().FreeAddress(bAddress);

        bNumEP = 1; //must have to be reset to 1

	bAddress = 0;
	ready = false;
	return 0;
}

uint32_t ADK::RcvData(uint8_t *bytes_rcvd, uint8_t *dataptr) {
        USBTRACE2("\r\nAddr: ", bAddress );
        USBTRACE2("\r\nEP: ",epInfo[epDataInIndex].epAddr);
        return pUsb->inTransfer(bAddress, epInfo[epDataInIndex].epAddr, bytes_rcvd, dataptr);
}

uint32_t ADK::SndData(uint32_t nbytes, uint8_t *dataptr) {
        return pUsb->outTransfer(bAddress, epInfo[epDataOutIndex].epAddr, nbytes, dataptr);
}

void ADK::PrintEndpointDescriptor(const USB_ENDPOINT_DESCRIPTOR* ep_ptr) {
        Notify(PSTR("Endpoint descriptor:"), 0x80);
        Notify(PSTR("\r\nLength:\t\t"), 0x80);
        PrintHex<uint8_t > (ep_ptr->bLength, 0x80);
        Notify(PSTR("\r\nType:\t\t"), 0x80);
        PrintHex<uint8_t > (ep_ptr->bDescriptorType, 0x80);
        Notify(PSTR("\r\nAddress:\t"), 0x80);
        PrintHex<uint8_t > (ep_ptr->bEndpointAddress, 0x80);
        Notify(PSTR("\r\nAttributes:\t"), 0x80);
        PrintHex<uint8_t > (ep_ptr->bmAttributes, 0x80);
        Notify(PSTR("\r\nMaxPktSize:\t"), 0x80);
        PrintHex<uint16_t > (ep_ptr->wMaxPacketSize, 0x80);
        Notify(PSTR("\r\nPoll Intrv:\t"), 0x80);
        PrintHex<uint8_t > (ep_ptr->bInterval, 0x80);
        Notify(PSTR("\r\n"), 0x80);
}
