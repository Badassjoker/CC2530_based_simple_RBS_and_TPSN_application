#include <math.h>
#include "OSAL.h"
#include "ZGlobals.h"
#include "AF.h"
#include "aps_groups.h"
#include "ZDApp.h"
#include "OSAL_Clock.h"
#include "SampleApp.h"
#include "SampleAppHw.h"
#include "OnBoard.h"
#include "hal_lcd.h"
#include "hal_led.h"
#include "hal_key.h"
#include  "MT_UART.h"

// This list should be filled with Application specific Cluster IDs.
const cId_t SampleApp_ClusterList[SAMPLEAPP_MAX_CLUSTERS] =
{
  SAMPLEAPP_PERIODIC_CLUSTERID,
  SAMPLEAPP_FLASH_CLUSTERID
};

const SimpleDescriptionFormat_t SampleApp_SimpleDesc =
{
  SAMPLEAPP_ENDPOINT,              //  int Endpoint;
  SAMPLEAPP_PROFID,                //  uint16 AppProfId[2];
  SAMPLEAPP_DEVICEID,              //  uint16 AppDeviceId[2];
  SAMPLEAPP_DEVICE_VERSION,        //  int   AppDevVer:4;
  SAMPLEAPP_FLAGS,                 //  int   AppFlags:4;
  SAMPLEAPP_MAX_CLUSTERS,          //  uint8  AppNumInClusters;
  (cId_t *)SampleApp_ClusterList,  //  uint8 *pAppInClusterList;
  SAMPLEAPP_MAX_CLUSTERS,          //  uint8  AppNumInClusters;
  (cId_t *)SampleApp_ClusterList   //  uint8 *pAppInClusterList;
};

// This is the Endpoint/Interface description.  It is defined here, but
// filled-in in SampleApp_Init().  Another way to go would be to fill
// in the structure here and make it a "const" (in code space).  The
// way it's defined in this sample app it is define in RAM.
endPointDesc_t SampleApp_epDesc;

uint8 SampleApp_TaskID;   // Task ID for internal task/event processing
                          // This variable will be received when
                          // SampleApp_Init() is called.
devStates_t SampleApp_NwkState;

uint8 SampleApp_TransID;  // This is the unique message ID (counter)

afAddrType_t SampleApp_Periodic_DstAddr;
afAddrType_t SampleApp_Flash_DstAddr;

aps_Group_t SampleApp_Group;

uint8 SampleAppPeriodicCounter = 0;
uint8 SampleAppFlashCounter = 0;

/*********************************************************************
 * LOCAL FUNCTIONS
 */
void SampleApp_HandleKeys( uint8 shift, uint8 keys );
void SampleApp_MessageMSGCB( afIncomingMSGPacket_t *pckt );
void SampleApp_SendPeriodicMessage( void );
void SampleApp_SendFlashMessage( uint16 flashTime );
void SampleApp_ConvertToT1(uint32 seconds);
void SampleApp_ConvertToDELTA(uint32 seconds);
void SampleApp_ConvertToSETTIME(uint32 seconds);
void SampleApp_ConvertToT6(uint32 seconds);
void SampleApp_SendClockMessage( void );
uint32 SampleApp_ConvertToT2T3(void);

void SampleApp_Init( uint8 task_id )
{
  SampleApp_TaskID = task_id;
  SampleApp_NwkState = DEV_INIT;
  SampleApp_TransID = 0;
  
  osal_setClock(0);
  
  MT_UartInit();//串口初始化
  MT_UartRegisterTaskID(task_id);//登记任务号
  //HalUARTWrite(0,"Hello World\n",12); //（串口0，'字符'，字符个数。）
  
  // Device hardware initialization can be added here or in main() (Zmain.c).
  // If the hardware is application specific - add it here.
  // If the hardware is other parts of the device add it in main().

 #if defined ( BUILD_ALL_DEVICES )
  // The "Demo" target is setup to have BUILD_ALL_DEVICES and HOLD_AUTO_START
  // We are looking at a jumper (defined in SampleAppHw.c) to be jumpered
  // together - if they are - we will start up a coordinator. Otherwise,
  // the device will start as a router.
  if ( readCoordinatorJumper() )
    zgDeviceLogicalType = ZG_DEVICETYPE_COORDINATOR;
  else
    zgDeviceLogicalType = ZG_DEVICETYPE_ROUTER;
#endif // BUILD_ALL_DEVICES

#if defined ( HOLD_AUTO_START )
  // HOLD_AUTO_START is a compile option that will surpress ZDApp
  //  from starting the device and wait for the application to
  //  start the device.
  ZDOInitDevice(0);
#endif

  // Setup for the periodic message's destination address
  // Broadcast to everyone
  SampleApp_Periodic_DstAddr.addrMode = (afAddrMode_t)AddrBroadcast;
  SampleApp_Periodic_DstAddr.endPoint = SAMPLEAPP_ENDPOINT;
  SampleApp_Periodic_DstAddr.addr.shortAddr = 0xFFFF;

  // Setup for the flash command's destination address - Group 1
  SampleApp_Flash_DstAddr.addrMode = (afAddrMode_t)afAddrGroup;
  SampleApp_Flash_DstAddr.endPoint = SAMPLEAPP_ENDPOINT;
  SampleApp_Flash_DstAddr.addr.shortAddr = SAMPLEAPP_FLASH_GROUP;

  // Fill out the endpoint description.
  SampleApp_epDesc.endPoint = SAMPLEAPP_ENDPOINT;
  SampleApp_epDesc.task_id = &SampleApp_TaskID;
  SampleApp_epDesc.simpleDesc
            = (SimpleDescriptionFormat_t *)&SampleApp_SimpleDesc;
  SampleApp_epDesc.latencyReq = noLatencyReqs;

  // Register the endpoint description with the AF
  afRegister( &SampleApp_epDesc );

  // Register for all key events - This app will handle all key events
  RegisterForKeys( SampleApp_TaskID );

  // By default, all devices start out in Group 1
  SampleApp_Group.ID = SAMPLEAPP_FLASH_GROUP;
  osal_memcpy( SampleApp_Group.name, "Group 1", 7  );
  aps_AddGroup( SAMPLEAPP_ENDPOINT, &SampleApp_Group );

#if defined ( LCD_SUPPORTED )
  HalLcdWriteString( "SampleApp", HAL_LCD_LINE_1 );
#endif
}

uint16 SampleApp_ProcessEvent( uint8 task_id, uint16 events )
{
  afIncomingMSGPacket_t *MSGpkt;
  (void)task_id;  // Intentionally unreferenced parameter

  if ( events & SYS_EVENT_MSG )
  {
    MSGpkt = (afIncomingMSGPacket_t *)osal_msg_receive( SampleApp_TaskID );
    while ( MSGpkt )
    {
      switch ( MSGpkt->hdr.event )
      {
        // Received when a key is pressed
        case KEY_CHANGE:
          SampleApp_HandleKeys( ((keyChange_t *)MSGpkt)->state, ((keyChange_t *)MSGpkt)->keys );
          break;

        // Received when a messages is received (OTA) for this endpoint
        case AF_INCOMING_MSG_CMD:
          SampleApp_MessageMSGCB( MSGpkt );
          break;

        // Received whenever the device changes state in the network
        case ZDO_STATE_CHANGE:
          SampleApp_NwkState = (devStates_t)(MSGpkt->hdr.status);
          if ( (SampleApp_NwkState == DEV_ZB_COORD)
              || (SampleApp_NwkState == DEV_ROUTER)
              || (SampleApp_NwkState == DEV_END_DEVICE) )
          {
            // Start sending the periodic message in a regular interval.
            osal_start_timerEx( SampleApp_TaskID,
                              SAMPLEAPP_SEND_PERIODIC_MSG_EVT,
                              SAMPLEAPP_SEND_PERIODIC_MSG_TIMEOUT );
          }
          else
          {
            // Device is no longer in the network
          }
          break;

        default:
          break;
      }

      // Release the memory
      osal_msg_deallocate( (uint8 *)MSGpkt );

      // Next - if one is available
      MSGpkt = (afIncomingMSGPacket_t *)osal_msg_receive( SampleApp_TaskID );
    }

    // return unprocessed events
    return (events ^ SYS_EVENT_MSG);
  }

  // Send a message out - This event is generated by a timer
  //  (setup in SampleApp_Init()).
  if ( events & SAMPLEAPP_SEND_PERIODIC_MSG_EVT )
  {
    // Send the periodic message
    SampleApp_SendPeriodicMessage();//周期性发送函数

    // Setup to send message again in normal period (+ a little jitter)
    osal_start_timerEx( SampleApp_TaskID, SAMPLEAPP_SEND_PERIODIC_MSG_EVT,
        (SAMPLEAPP_SEND_PERIODIC_MSG_TIMEOUT + (osal_rand() & 0x00FF)) );

    // return unprocessed events
    return (events ^ SAMPLEAPP_SEND_PERIODIC_MSG_EVT);
  }

  // Discard unknown events
  return 0;
}

void SampleApp_HandleKeys( uint8 shift, uint8 keys )
{
  (void)shift;  // Intentionally unreferenced parameter
  
  if ( keys & HAL_KEY_SW_1 )
  {
    /* This key sends the Flash Command is sent to Group 1.
     * This device will not receive the Flash Command from this
     * device (even if it belongs to group 1).
     */
    SampleApp_SendFlashMessage( SAMPLEAPP_FLASH_DURATION );
  }

  if ( keys & HAL_KEY_SW_2 )
  {
    /* The Flashr Command is sent to Group 1.
     * This key toggles this device in and out of group 1.
     * If this device doesn't belong to group 1, this application
     * will not receive the Flash command sent to group 1.
     */
    aps_Group_t *grp;
    grp = aps_FindGroup( SAMPLEAPP_ENDPOINT, SAMPLEAPP_FLASH_GROUP );
    if ( grp )
    {
      // Remove from the group
      aps_RemoveGroup( SAMPLEAPP_ENDPOINT, SAMPLEAPP_FLASH_GROUP );
    }
    else
    {
      // Add to the flash group
      aps_AddGroup( SAMPLEAPP_ENDPOINT, &SampleApp_Group );
    }
  }
}

unsigned char T1[10];
unsigned char T2T3[10];
unsigned char DELTA[10];
unsigned char SETTIME[10];
unsigned char T6[10];
uint32 t1;
uint32 t2;
uint32 t3;
uint32 t4;
uint32 t5;
uint32 t6;
uint32 currentTime;
uint32 delta;
uint32 setTime;

void SampleApp_MessageMSGCB( afIncomingMSGPacket_t *pkt )
{
  switch ( pkt->clusterId )
  {
    case SAMPLEAPP_PERIODIC_CLUSTERID:
      for (int i = 0; i < 10; i++)
        T2T3[i] = pkt->cmd.Data[i];
      t4 = osal_getClock();
      t2 = SampleApp_ConvertToT2T3() / 2;
      t3 = t2;
      if ((t2 + t3) > (t1 + t4))
      {
        delta = ((t2 + t3) - (t1 + t4)) / 2;
        currentTime = osal_getClock();
        setTime = currentTime + delta;
        osal_setClock(setTime);
      }
      else
      {
        delta = ((t1 + t4) - (t2 + t3)) / 2;
        currentTime = osal_getClock();
        setTime = currentTime - delta;
        osal_setClock(setTime);
      }
      SampleApp_ConvertToDELTA(delta);
      SampleApp_ConvertToSETTIME(setTime);
      HalUARTWrite(0,"DELTA\n",6);
      HalUARTWrite(0, DELTA,10);
      HalUARTWrite(0,"\n",1);
      HalUARTWrite(0,"SETTIME\n",8);
      HalUARTWrite(0, SETTIME,10);
      HalUARTWrite(0,"\n",1);
      break;

    case SAMPLEAPP_FLASH_CLUSTERID:
      t5 = osal_getClock();
      t6 = osal_getClock();
      t6 += t5;
      SampleApp_ConvertToT6(t6);
      SampleApp_SendClockMessage();
      break;
  }
}

void SampleApp_SendPeriodicMessage( void )
{
  t1 = osal_getClock();
  SampleApp_ConvertToT1(t1);
  if ( AF_DataRequest( &SampleApp_Periodic_DstAddr, &SampleApp_epDesc,
                       SAMPLEAPP_PERIODIC_CLUSTERID,
                       10,//字节数
                       T1,//指针头
                       &SampleApp_TransID,
                       AF_DISCV_ROUTE,
                       AF_DEFAULT_RADIUS ) == afStatus_SUCCESS )
  {
  }
  else
  {
    // Error occurred in request to send.
  }
}

void SampleApp_SendFlashMessage( uint16 flashTime )
{
  uint8 buffer[3];
  buffer[0] = (uint8)(SampleAppFlashCounter++);
  buffer[1] = LO_UINT16( flashTime );
  buffer[2] = HI_UINT16( flashTime );

  if ( AF_DataRequest( &SampleApp_Flash_DstAddr, &SampleApp_epDesc,
                       SAMPLEAPP_FLASH_CLUSTERID,
                       3,
                       buffer,
                       &SampleApp_TransID,
                       AF_DISCV_ROUTE,
                       AF_DEFAULT_RADIUS ) == afStatus_SUCCESS )
  {
  }
  else
  {
    // Error occurred in request to send.
  }
}

void SampleApp_ConvertToT1(uint32 seconds)
{
  uint8 tempUsing = 0;
  
  for (int i = 10; i > 0; i--)
  {
    tempUsing = seconds % 10;
    seconds /= 10;
    if (tempUsing == 0)
      T1[i - 1] = '0';
    else if (tempUsing == 1)
      T1[i - 1] = '1';
    else if (tempUsing == 2)
      T1[i - 1] = '2';
    else if (tempUsing == 3)
      T1[i - 1] = '3';
    else if (tempUsing == 4)
      T1[i - 1] = '4';
    else if (tempUsing == 5)
      T1[i - 1] = '5';
    else if (tempUsing == 6)
      T1[i - 1] = '6';
    else if (tempUsing == 7)
      T1[i - 1] = '7';
    else if (tempUsing == 8)
      T1[i - 1] = '8';
    else
      T1[i - 1] = '9';
  }
}

void SampleApp_ConvertToDELTA(uint32 seconds)
{
  uint8 tempUsing = 0;
  
  for (int i = 10; i > 0; i--)
  {
    tempUsing = seconds % 10;
    seconds /= 10;
    if (tempUsing == 0)
      DELTA[i - 1] = '0';
    else if (tempUsing == 1)
      DELTA[i - 1] = '1';
    else if (tempUsing == 2)
      DELTA[i - 1] = '2';
    else if (tempUsing == 3)
      DELTA[i - 1] = '3';
    else if (tempUsing == 4)
      DELTA[i - 1] = '4';
    else if (tempUsing == 5)
      DELTA[i - 1] = '5';
    else if (tempUsing == 6)
      DELTA[i - 1] = '6';
    else if (tempUsing == 7)
      DELTA[i - 1] = '7';
    else if (tempUsing == 8)
      DELTA[i - 1] = '8';
    else
      DELTA[i - 1] = '9';
  }
}

void SampleApp_ConvertToSETTIME(uint32 seconds)
{
  uint8 tempUsing = 0;
  
  for (int i = 10; i > 0; i--)
  {
    tempUsing = seconds % 10;
    seconds /= 10;
    if (tempUsing == 0)
      SETTIME[i - 1] = '0';
    else if (tempUsing == 1)
      SETTIME[i - 1] = '1';
    else if (tempUsing == 2)
      SETTIME[i - 1] = '2';
    else if (tempUsing == 3)
      SETTIME[i - 1] = '3';
    else if (tempUsing == 4)
      SETTIME[i - 1] = '4';
    else if (tempUsing == 5)
      SETTIME[i - 1] = '5';
    else if (tempUsing == 6)
      SETTIME[i - 1] = '6';
    else if (tempUsing == 7)
      SETTIME[i - 1] = '7';
    else if (tempUsing == 8)
      SETTIME[i - 1] = '8';
    else
      SETTIME[i - 1] = '9';
  }
}

void SampleApp_ConvertToT6(uint32 seconds)
{
  uint8 tempUsing = 0;
  
  for (int i = 10; i > 0; i--)
  {
    tempUsing = seconds % 10;
    seconds /= 10;
    if (tempUsing == 0)
      T6[i - 1] = '0';
    else if (tempUsing == 1)
      T6[i - 1] = '1';
    else if (tempUsing == 2)
      T6[i - 1] = '2';
    else if (tempUsing == 3)
      T6[i - 1] = '3';
    else if (tempUsing == 4)
      T6[i - 1] = '4';
    else if (tempUsing == 5)
      T6[i - 1] = '5';
    else if (tempUsing == 6)
      T6[i - 1] = '6';
    else if (tempUsing == 7)
      T6[i - 1] = '7';
    else if (tempUsing == 8)
      T6[i - 1] = '8';
    else
      T6[i - 1] = '9';
  }
}

void SampleApp_SendClockMessage( void )
{
  if ( AF_DataRequest( &SampleApp_Flash_DstAddr,
                       &SampleApp_epDesc,
                       SAMPLEAPP_FLASH_CLUSTERID,
                       10,//字节数
                       T6,//指针头
                       &SampleApp_TransID,
                       AF_DISCV_ROUTE,
                       AF_DEFAULT_RADIUS ) == afStatus_SUCCESS )
  {
  }
  else
  {
    // Error occurred in request to send.
  }
}

uint32 SampleApp_ConvertToT2T3(void)
{
  uint32 seconds = 0;
  for (int i = 10; i > 0; i--)
  {
    if (T2T3[i - 1] == '1')
      seconds += 1 * pow(10, 10 - i);
    else if (T2T3[i - 1] == '2')
      seconds += 2 * pow(10, 10 - i);
    else if (T2T3[i - 1] == '3')
      seconds += 3 * pow(10, 10 - i);
    else if (T2T3[i - 1] == '4')
      seconds += 4 * pow(10, 10 - i);
    else if (T2T3[i - 1] == '5')
      seconds += 5 * pow(10, 10 - i);
    else if (T2T3[i - 1] == '6')
      seconds += 6 * pow(10, 10 - i);
    else if (T2T3[i - 1] == '7')
      seconds += 7 * pow(10, 10 - i);
    else if (T2T3[i - 1] == '8')
      seconds += 8 * pow(10, 10 - i);
    else if (T2T3[i - 1] == '9')
      seconds += 9 * pow(10, 10 - i);
    else;
  }
  return seconds;
}
