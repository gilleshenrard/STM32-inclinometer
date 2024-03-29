/**
 * @file SSD1306.c
 * @brief Implement the functioning of the SSD1306 OLED screen via SPI and DMA
 * @author Gilles Henrard
 * @date 09/03/2024
 *
 * @note Datasheet : https://cdn-shop.adafruit.com/datasheets/SSD1306.pdf
 */
#include "SSD1306.h"
#include "numbersVerdana16.h"
#include "SSD1306_registers.h"
#include "icons.h"
#include <assert.h>

//definitions
#define REFTYPE_PAGE        SSD_LAST_PAGE
#define REFTYPE_COLUMN      (SSD_LAST_COLUMN - REFERENCETYPE_NB_BYTES)
#define SPI_TIMEOUT_MS		10U		///< Maximum number of milliseconds SPI traffic should last before timeout
#define MAX_DATA_SIZE		1024U	///< Maximum SSD1306 data size (128 * 64 pixels / 8 pixels per byte)
#define ANGLE_NB_CHARS		6U		///< Number of characters in the angle array
#define SSD_LAST_COLUMN		127U	///< Index of the highest column
#define SSD_LAST_PAGE		31U		///< Index of the highest page

//static assertions (ran at compile time)
_Static_assert((ANGLE_NB_CHARS * VERDANA_NB_BYTES_CHAR) <= MAX_DATA_SIZE, "SSD1306 font chosen uses too much space.");
_Static_assert((ANGLE_NB_CHARS * VERDANA_CHAR_WIDTH) <= (SSD_LAST_COLUMN + 1), "SSD1306 font chosen has too many columns.");

/**
 * @brief Enumeration of the function IDs of the SSD1306
 */
typedef enum _SSD1306functionCodes_e{
    INIT = 0,		///< SSD1306initialise()
    SEND_CMD,		///< SSD1306sendCommand()
    PRT_ANGLE,		///< SSD1306_printAngleTenths()
    SENDING_DATA,	///< stSendingData()
    WAITING_DMA_RDY	///< stWaitingForTXdone()
}_SSD1306functionCodes_e;

/**
 * @brief SPI Data/command pin status enumeration
 */
typedef enum{
    COMMAND = 0,
    DATA,
}DCgpio_e;

/**
 * @brief Screen state machine state prototype
 *
 * @return Return code of the state
 */
typedef errorCode_u (*screenState)();

//communication functions with the SSD1306
static inline void setDataCommandGPIO(DCgpio_e function);
static errorCode_u sendCommand(SSD1306register_e regNumber, const uint8_t parameters[], uint8_t nbParameters);

//state machine
static errorCode_u stIdle();
static errorCode_u stConfiguring();
static errorCode_u stSendingData();
static errorCode_u stWaitingForTXdone();

//state variables
volatile uint16_t			screenTimer_ms = 0;				///< Timer used with screen SPI transmissions (in ms)
volatile uint16_t			ssd1306SPITimer_ms = 0;			///< Timer used to make sure SPI does not time out (in ms)
static SPI_TypeDef*			_spiHandle = (void*)0;			///< SPI handle used with the SSD1306
static DMA_TypeDef*			_dmaHandle = (void*)0;			///< DMA handle used with the SSD1306
static uint32_t				_dmaChannel = 0x00000000U;		///< DMA channel used
static screenState			_state = stConfiguring;			///< State machine current state
static uint8_t				_screenBuffer[MAX_DATA_SIZE];	///< Buffer used to send data to the screen
static uint8_t 				_limitColumns[2];				///< Buffer used to set the first and last column to send
static uint8_t				_limitPages[2];					///< Buffer used to set the first and last page to send
static uint16_t				_size;							///< Number of bytes to send


/********************************************************************************************************************************************/
/********************************************************************************************************************************************/


/**
 * @brief Initialise the SSD1306
 *
 * @param handle SPI handle used
 * @return Success
 * @retval 1	Error while initialising the registers
 * @retval 2	Error while clearing the screen
 */
errorCode_u SSD1306initialise(SPI_TypeDef* handle, DMA_TypeDef* dma, uint32_t dmaChannel){
    _spiHandle = handle;
    _dmaHandle = dma;
    _dmaChannel = dmaChannel;

    //make sure to disable SSD1306 SPI communication
    LL_SPI_Disable(_spiHandle);
    LL_DMA_DisableChannel(_dmaHandle, _dmaChannel);

    //set the DMA source and destination addresses (will always use the same ones)
    LL_DMA_ConfigAddresses(_dmaHandle, _dmaChannel, (uint32_t)&_screenBuffer, LL_SPI_DMA_GetRegAddr(_spiHandle), LL_DMA_DIRECTION_MEMORY_TO_PERIPH);

    return (ERR_SUCCESS);
}

/**
 * brief Set the Data/Command pin
 *
 * @param function Value of the data/command pin
 */
static inline void setDataCommandGPIO(DCgpio_e function){
    if(function == COMMAND)
        LL_GPIO_ResetOutputPin(SSD1306_DC_GPIO_Port, SSD1306_DC_Pin);
    else
        LL_GPIO_SetOutputPin(SSD1306_DC_GPIO_Port, SSD1306_DC_Pin);
}

/**
 * @brief Send a command with parameters
 *
 * @param regNumber Register number
 * @param parameters Parameters to write
 * @param nbParameters Number of parameters to write
 * @return Success
 * @retval 1	Number of parameters above maximum
 * @retval 2	Timeout while sending the command
 */
errorCode_u sendCommand(SSD1306register_e regNumber, const uint8_t parameters[], uint8_t nbParameters){
    static const uint8_t MAX_PARAMETERS = 6U;		///< Maximum number of parameters a command can have
    errorCode_u result = ERR_SUCCESS;

    //assertions
    assert(_spiHandle);						//handle is not null
    assert(parameters || !nbParameters);	//either 0 parameters, or parameters array not null

    //if too many parameters, error
    if(nbParameters > MAX_PARAMETERS)
        return(createErrorCode(SEND_CMD, 1, ERR_WARNING));

    //set command pin and enable SPI
    ssd1306SPITimer_ms = SPI_TIMEOUT_MS;
    setDataCommandGPIO(COMMAND);
    LL_SPI_Enable(_spiHandle);

    //send the command byte
    LL_SPI_TransmitData8(_spiHandle, regNumber);

    //send the parameters
    uint8_t* iterator = (uint8_t*)parameters;
    while(nbParameters && ssd1306SPITimer_ms){
        //wait for the previous byte to be done, then send the next one
        while(!LL_SPI_IsActiveFlag_TXE(_spiHandle) && ssd1306SPITimer_ms);
        if(ssd1306SPITimer_ms)
            LL_SPI_TransmitData8(_spiHandle, *iterator);

        iterator++;
        nbParameters--;
    }

    //wait for transaction to be finished and clear Overrun flag
    while(LL_SPI_IsActiveFlag_BSY(_spiHandle) && ssd1306SPITimer_ms);
    LL_SPI_ClearFlag_OVR(_spiHandle);

    //disable SPI and return status
    LL_SPI_Disable(_spiHandle);

    //if timeout, error
    if(!ssd1306SPITimer_ms)
        return (createErrorCode(SEND_CMD, 2, ERR_WARNING));

    return (result);
}

/**
 * @brief Wipe the screen blank and draw the separator and icons
 *
 * @return Success
 */
errorCode_u SSD1306drawBaseScreen(){
    uint8_t* iterator = _screenBuffer;
    uint16_t i;

    //define the whole screen as the drawing window
    _limitColumns[0] = 0;
    _limitColumns[1] = SSD_LAST_COLUMN;
    _limitPages[0] = 0;
    _limitPages[1] = SSD_LAST_PAGE;
    _size = MAX_DATA_SIZE;

    //fill the screen buffer with blank pixels value
    for(i = 0 ; i < MAX_DATA_SIZE ; i++)
        *(iterator++) = 0x00U;

    //draw the middle screen separator in the buffer (avoid drawing in the arrows icon zone)
    static const uint8_t LIGN = 0x03U;
    iterator = &_screenBuffer[(MAX_DATA_SIZE >> 1) + ARROWSICON_WIDTH];
    for(i = ARROWSICON_WIDTH ; i <= SSD_LAST_COLUMN ; i++)
        *(iterator++) = LIGN;

    //draw the arrows icon
    for(i = 0 ; i < ARROWSICON_NB_BYTES ; i++){
        //copy a byte, while making sure to copy it at the proper page number
        uint16_t bufferOffset = (i / ARROWSICON_WIDTH) * (SSD_LAST_COLUMN + 1);
        _screenBuffer[bufferOffset + (i % ARROWSICON_WIDTH)] = arrowsIcon_32px[i];
    }

    //draw the absolute referential icon
    iterator = &_screenBuffer[MAX_DATA_SIZE - REFERENCETYPE_NB_BYTES];
    for(i = 0 ; i < REFERENCETYPE_NB_BYTES ; i++)
        *(iterator++) = absoluteReferentialIcon[i];

    _state = stSendingData;
    return (ERR_SUCCESS);
}

/**
 * @brief Check if the screen is ready to accept new commands
 *
 * @return 0 Not ready
 * @retval 1 Ready
 */
uint8_t isScreenReady(){
    return (_state == stIdle);
}

/**
 * @brief Print an angle (in degrees, with sign) on the screen
 *
 * @param angleTenths	Angle to print
 * @param rotationAxis  Axis around which the rotation angle is to print
 *
 * @return Success
 * @retval 1	Angle above maximum amplitude
 */
errorCode_u SSD1306_printAngleTenths(int16_t angleTenths, rotationAxis_e rotationAxis){
    static const uint8_t  ANGLE_COLUMN = 40U;		    ///< Column number of the first screen line
    static const uint8_t ANGLE_ROLL_PAGE = 1U;		    ///< Number of the page at which display the roll axis angle
    static const uint8_t ANGLE_PITCH_PAGE = 5U;		    ///< Number of the page at which display the pitch axis angle
    static const int16_t MIN_ANGLE_DEG_TENTHS = -900;	///< Minimum angle allowed (in tenths of degrees)
    static const int16_t MAX_ANGLE_DEG_TENTHS =  900;	///< Maximum angle allowed (in tenths of degrees)
    static const uint8_t INDEX_SIGN = 0;	            ///< Index of the sign in the angle indexes array
    static const uint8_t INDEX_TENS = 1U;	            ///< Index of the tens in the angle indexes array
    static const uint8_t INDEX_UNITS = 2U;	            ///< Index of the units in the angle indexes array
    static const uint8_t INDEX_TENTHS = 4U;	            ///< Index of the tenths in the angle indexes array
    uint8_t charIndexes[ANGLE_NB_CHARS] = {INDEX_PLUS, 0, 0, INDEX_DOT, 0, INDEX_DEG};
    uint8_t* iterator = _screenBuffer;

    //clamp the angle to print to the min value
    if(angleTenths < MIN_ANGLE_DEG_TENTHS)
        angleTenths = MIN_ANGLE_DEG_TENTHS;

    //clamp the angle to print to the max value
    if(angleTenths > MAX_ANGLE_DEG_TENTHS)
        angleTenths = MAX_ANGLE_DEG_TENTHS;

    //if angle negative, replace plus sign with minus sign
    if(angleTenths < 0){
        charIndexes[INDEX_SIGN] = INDEX_MINUS;
        angleTenths = -angleTenths;
    }

    //store the values
    _limitColumns[0] = ANGLE_COLUMN;
    _limitColumns[1] = ANGLE_COLUMN + (VERDANA_CHAR_WIDTH * ANGLE_NB_CHARS) - 1;
    _limitPages[0] = (rotationAxis == ROLL ? ANGLE_ROLL_PAGE : ANGLE_PITCH_PAGE);
    _limitPages[1] = (rotationAxis == ROLL ? ANGLE_ROLL_PAGE : ANGLE_PITCH_PAGE) + 1;
    _size = ANGLE_NB_CHARS * VERDANA_NB_BYTES_CHAR;

    //fill the angle characters indexes array with the float values (tens, units, tenths)
    charIndexes[INDEX_TENS] = (uint8_t)(angleTenths / 100);
    charIndexes[INDEX_UNITS] = (uint8_t)((angleTenths / 10) % 10);
    charIndexes[INDEX_TENTHS] = (uint8_t)(angleTenths % 10);

    //fill the buffer with all the required bitmaps bytes (column by column, then character by character, then page by page)
    for(uint8_t page = 0 ; page < VERDANA_NB_PAGES ; page++){
        for(uint8_t character = 0 ; character < ANGLE_NB_CHARS ; character++){
            for(uint8_t column = 0 ; column < VERDANA_CHAR_WIDTH ; column++){
                *iterator = verdana_16ptNumbers[charIndexes[character]][(VERDANA_CHAR_WIDTH * page) + column];
                iterator++;
            }
        }
    }

    //get to printing state
    _state = stSendingData;
    return (ERR_SUCCESS);
}

/**
 * @brief Draw the icon representing the type of referential currently used
 * 
 * @param type Referential type
 * @return Success
 */
errorCode_u SSD1306_printReferentialIcon(referentialType_e type){
    uint8_t* iterator = _screenBuffer;
    const uint8_t* iconIterator = (type == ABSOLUTE ? absoluteReferentialIcon : relativeReferentialIcon);

    _limitColumns[0] = REFTYPE_COLUMN;
    _limitColumns[1] = REFTYPE_COLUMN + REFERENCETYPE_NB_BYTES;
    _limitPages[0] = REFTYPE_PAGE;
    _limitPages[1] = REFTYPE_PAGE;
    _size = REFERENCETYPE_NB_BYTES;

    for(uint8_t i = 0 ; i < REFERENCETYPE_NB_BYTES ; i++)
        *(iterator++) = *(iconIterator++);

    //get to printing state
    _state = stSendingData;
    return (ERR_SUCCESS);
}

/**
 * @brief Draw/erase the icon representing the hold function
 * 
 * @param status 1 to print, 0 to erase
 * @return Success
 */
errorCode_u SSD1306_printHoldIcon(uint8_t status){
    uint8_t* iterator = _screenBuffer;
    const uint8_t* iconIterator = holdIcon;

    _limitColumns[0] = REFTYPE_COLUMN - REFERENCETYPE_NB_BYTES;
    _limitColumns[1] = REFTYPE_COLUMN;
    _limitPages[0] = REFTYPE_PAGE;
    _limitPages[1] = REFTYPE_PAGE;
    _size = REFERENCETYPE_NB_BYTES;

    for(uint8_t i = 0 ; i < REFERENCETYPE_NB_BYTES ; i++)
        if(status)
            *(iterator++) = *(iconIterator++);
        else
            *(iterator++) = 0x00;

    //get to printing state
    _state = stSendingData;
    return (ERR_SUCCESS);
}

/**
 * @brief Run the state machine
 *
 * @return Return code of the current state
 */
errorCode_u SSD1306update(){
    return ((*_state)());
}


/********************************************************************************************************************************************/
/********************************************************************************************************************************************/


/**
 * @brief State in which the SSD1306 configuration registers are set
 * 
 * @return Success
 * @retval 1	Error while setting a configuration register
 * @retval 2	Error while requesting the screen wipe
 */
static errorCode_u stConfiguring(){
    #define NB_INIT_REGISERS    8U		                                ///< Number of registers set at initialisation
    static const uint8_t initCommands[NB_INIT_REGISERS][3] = {			///< Array used to initialise the registers
        {SCAN_DIRECTION_N1_0,	0,	0x00},
        {HARDWARE_CONFIG,		1,	SSD_PIN_CONFIG_ALT | SSD_COM_REMAP_DISABLE},
        {SEGMENT_REMAP_127,		0,	0x00},
        {MEMORY_ADDR_MODE,		1,	SSD_HORIZONTAL_ADDR},
        {CONTRAST_CONTROL,		1,	SSD_CONTRAST_HIGHEST},
        {CLOCK_DIVIDE_RATIO,	1,	SSD_CLOCK_FREQ_MID | SSD_CLOCK_DIVIDER_1},
        {CHG_PUMP_REGULATOR,	1,	SSD_ENABLE_CHG_PUMP},
        {DISPLAY_ON,			0,	0x00},
    };
    errorCode_u result;

    //reset the chip
    LL_GPIO_ResetOutputPin(SSD1306_RES_GPIO_Port, SSD1306_RES_Pin);
    LL_GPIO_SetOutputPin(SSD1306_RES_GPIO_Port, SSD1306_RES_Pin);

    //initialisation taken from PDF p. 64 (Application Example)
    //	values which don't change from reset values aren't modified
    //TODO test for max oscillator frequency
    for(uint8_t i = 0 ; i < NB_INIT_REGISERS ; i++){
        result = sendCommand(initCommands[i][0], &initCommands[i][2], initCommands[i][1]);
        if(isError(result))
            return (pushErrorCode(result, INIT, 1));
    }

    result = SSD1306drawBaseScreen();
    if(isError(result))
        return (pushErrorCode(result, INIT, 2));

    _state = stSendingData;
    return (ERR_SUCCESS);
}

/**
 * @brief State in which the screen awaits for commands
 *
 * @return Success
 */
errorCode_u stIdle(){
    return (ERR_SUCCESS);
}

/**
 * @brief State in which data is sent to the screen
 *
 * @return Success
 * @retval 1	Error occurred while sending the column address command
 * @retval 2	Error occurred while sending the page address command
 */
errorCode_u stSendingData(){
    errorCode_u result;

    //send the set start and end column addresses
    result = sendCommand(COLUMN_ADDRESS, _limitColumns, 2);
    if(isError(result)){
        _state = stIdle;
        return (pushErrorCode(result, SENDING_DATA, 1));
    }

    //send the set start and end page addresses
    result = sendCommand(PAGE_ADDRESS, _limitPages, 2);
    if(isError(result)){
        _state = stIdle;
        return (pushErrorCode(result, SENDING_DATA, 2));
    }

    //set data GPIO and enable SPI
    setDataCommandGPIO(DATA);
    LL_SPI_Enable(_spiHandle);

    //configure the DMA transaction
    LL_DMA_DisableChannel(_dmaHandle, _dmaChannel);
    LL_DMA_ClearFlag_GI5(_dmaHandle);
    LL_DMA_SetDataLength(_dmaHandle, _dmaChannel, _size);
    LL_DMA_EnableChannel(_dmaHandle, _dmaChannel);

    //send the data
    screenTimer_ms = SPI_TIMEOUT_MS;
    LL_SPI_EnableDMAReq_TX(_spiHandle);

    //get to next
    _state = stWaitingForTXdone;
    return (ERR_SUCCESS);
}

/**
 * @brief State in which the machine waits for a DMA transmission to end
 *
 * @return Success
 * @retval 1	Timeout while waiting for transmission to end
 * @retval 2	Error interrupt occurred during the DMA transfer
 */
errorCode_u stWaitingForTXdone(){
    errorCode_u result = ERR_SUCCESS;

    //if timer elapsed, stop DMA and error
    if(!screenTimer_ms){
        result = createErrorCode(WAITING_DMA_RDY, 1, ERR_ERROR);
        goto finalise;
    }

    //if DMA error, error
    if(LL_DMA_IsActiveFlag_TE5(_dmaHandle)){
        result = createErrorCode(WAITING_DMA_RDY, 2, ERR_ERROR);
        goto finalise;
    }

    //if transmission not complete yet, exit
    if(!LL_DMA_IsActiveFlag_TC5(_dmaHandle))
        return (ERR_SUCCESS);

finalise:
    LL_DMA_DisableChannel(_dmaHandle, _dmaChannel);
    LL_SPI_Disable(_spiHandle);
    _state = stIdle;
    return result;
}
