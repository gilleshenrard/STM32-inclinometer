/**
 * @brief Implement the ADXL345 accelerometer communication
 * @author Gilles Henrard
 * @date 01/03/2024
 *
 * @note Additional information can be found in :
 *   - ADXL345 datasheet : https://www.analog.com/media/en/technical-documentation/data-sheets/ADXL345.pdf
 *   - AN-1077 (Quick Start Guide) : https://www.analog.com/media/en/technical-documentation/application-notes/AN-1077.pdf
 *   - AN-1025 (FIFO application note) document : https://www.analog.com/media/en/technical-documentation/application-notes/AN-1025.pdf
 *
 */
#include "ADXL345.h"
#include "ADXL345registers.h"
#include "main.h"
#include <math.h>

//definitions
#define SPI_TIMEOUT_MS		10U				///< SPI direct transmission timeout span in milliseconds
#define INT_TIMEOUT_MS		1000U			///< Maximum number of milliseconds before watermark int. timeout
#define NB_REG_INIT			6U				///< Number of registers configured at initialisation
#define ADXL_AVG_SAMPLES	ADXL_SAMPLES_32	///< Amount of samples to integrate in the ADXL
#define ADXL_AVG_SHIFT		5U				///< Number used to shift the samples sum in order to divide it during integration

//assertions
static_assert((ADXL_AVG_SAMPLES >> ADXL_AVG_SHIFT) == 1, "ADXL_AVG_SHIFT does not divide all the samples configured with ADXL_AVG_SAMPLES");

//type definitions

/**
 * @brief Enumeration of the function IDs of the ADXL345
 */
typedef enum _ADXLfunctionCodes_e{
    INIT = 0,      		///< ADXL345initialise()
    SELF_TESTING_OFF,	///< stMeasuringST_OFF()
    SELF_TEST_WAIT,		///< stWaitingForSTenabled()
    SELF_TESTING_ON,	///< stMeasuringST_ON()
    MEASURE,        	///< stMeasuring()
    CHK_MEASURES,  		///< ADXL345hasNewMeasurements()
    WRITE_REGISTER,		///< ADXL345writeRegister()
    READ_REGISTERS,		///< ADXL345readRegisters()
    GET_X_ANGLE,		///< ADXL345getXangleDegrees()
    GET_Y_ANGLE,		///< ADXL345getYangleDegrees()
    INTEGRATE,			///< integrateFIFO()
    POP_FIFO,			///< popAndAddFIFO()
    STARTUP				///< stStartup()
}ADXLfunctionCodes_e;

/**
 * @brief State machine state prototype
 *
 * @return Error code of the state
 */
typedef errorCode_u (*adxlState)();

//machine state
static errorCode_u stStartup();
static errorCode_u stConfiguring();
static errorCode_u stMeasuringST_OFF();
static errorCode_u stWaitingForSTenabled();
static errorCode_u stMeasuringST_ON();
static errorCode_u stMeasuring();
static errorCode_u stError();

//manipulation functions
static errorCode_u writeRegister(adxl345Registers_e registerNumber, uint8_t value);
static errorCode_u readRegisters(adxl345Registers_e firstRegister, uint8_t value[], uint8_t size);
static errorCode_u integrateFIFO(int32_t values[]);

//tool functions
static inline uint8_t isFIFOdataReady();
static inline int16_t twoComplement(const uint8_t bytes[2]);

// Default DATA FORMAT (register 0x31) and FIFO CONTROL (register 0x38) register values
static const uint8_t DATA_FORMAT_DEFAULT = (ADXL_NO_SELF_TEST | ADXL_SPI_4WIRE | ADXL_INT_ACTIV_LOW | ADXL_13BIT_RESOL | ADXL_RIGHT_JUSTIFY | ADXL_RANGE_16G);
static const uint8_t FIFO_CONTROL_DEFAULT = (ADXL_MODE_FIFO | ADXL_INT_MAP_INT1 | (ADXL_AVG_SAMPLES - 1));

//global variables
volatile uint16_t			adxlTimer_ms = INT_TIMEOUT_MS;	///< Timer used in various states of the ADXL (in ms)
volatile uint16_t			adxlSPITimer_ms = 0;			///< Timer used to make sure SPI does not time out (in ms)

//state variables
static SPI_TypeDef*		_spiHandle = NULL;			///< SPI handle used with the ADXL345
static adxlState		_state = stStartup;			///< State machine current state
static uint8_t			_measurementsUpdated = 0;	///< Flag used to indicate new integrated measurements are ready within the ADXL345
static int32_t			_latestValues[NB_AXIS];		///< Array of latest axis values
static int32_t			_previousValues[NB_AXIS];	///< Array of values previously compared to latest
static int32_t			_zeroValues[NB_AXIS];	    ///< Array of values used to compensate measurements since last zeroing
static errorCode_u 		_result;					///< Variables used to store error codes


/********************************************************************************************************************************************/
/********************************************************************************************************************************************/


/**
 * @brief Initialise the ADXL345
 *
 * @param handle	SPI handle used
 * @returns 		Success
 */
errorCode_u ADXL345initialise(const SPI_TypeDef* handle){
    _spiHandle = (SPI_TypeDef*)handle;
    LL_SPI_Disable(_spiHandle);

    //reset all values
    for(uint8_t axis = 0 ; axis < NB_AXIS ; axis++){
        _latestValues[axis] = 0;
        _previousValues[axis] = 0;
        _zeroValues[axis] = 0;
    }

    return (ERR_SUCCESS);
}

/**
 * @brief Run the ADXL state machine
 *
 * @return Current machine state return value
 */
errorCode_u ADXL345update(){
    return ( (*_state)() );
}

/**
 * @brief Check if new measurements have been updated
 *
 * @retval 0 No new values available
 * @retval 1 New values are available
 */
uint8_t ADXL345hasChanged(axis_e axis){
    uint8_t tmp = (_latestValues[axis] != _previousValues[axis]);
    _previousValues[axis] = _latestValues[axis];

    return (tmp);
}

/**
 * @brief Write a single register on the ADXL345
 *
 * @param registerNumber Register number
 * @param value Register value
 * @return	 Success
 * @retval 1 Register number out of range
 * @retval 2 Timeout
 */
static errorCode_u writeRegister(adxl345Registers_e registerNumber, uint8_t value){
    //assertions
    assert(_spiHandle);

    //if register number above known or within the reserved range, error
    if((registerNumber > ADXL_REGISTER_MAXNB) || ((uint8_t)(registerNumber - 1) < ADXL_HIGH_RESERVED_REG))
        return (createErrorCode(WRITE_REGISTER, 1, ERR_WARNING));

    //set timeout timer and enable SPI
    adxlSPITimer_ms = SPI_TIMEOUT_MS;
    LL_SPI_Enable(_spiHandle);

    //send the write instruction
    LL_SPI_TransmitData8(_spiHandle, ADXL_WRITE | ADXL_SINGLE | registerNumber);

    //wait for TX buffer to be ready and send value to write
    while(!LL_SPI_IsActiveFlag_TXE(_spiHandle) && adxlSPITimer_ms);
    if(adxlSPITimer_ms)
        LL_SPI_TransmitData8(_spiHandle, value);

    //wait for transaction to be finished and clear Overrun flag
    while(LL_SPI_IsActiveFlag_BSY(_spiHandle) && adxlSPITimer_ms);
    LL_SPI_ClearFlag_OVR(_spiHandle);

    //disable SPI
    LL_SPI_Disable(_spiHandle);

    //if timeout, error
    if(!adxlSPITimer_ms)
        return (createErrorCode(WRITE_REGISTER, 2, ERR_WARNING));

    return (ERR_SUCCESS);
}

/**
 * @brief Read several registers on the ADXL345
 *
 * @param firstRegister Number of the first register to read
 * @param[out] value Registers value array
 * @param size Number of registers to read
 * @return   Success
 * @retval 1 Register number out of range
 * @retval 2 Timeout
 */
static errorCode_u readRegisters(adxl345Registers_e firstRegister, uint8_t value[], uint8_t size){
    static const uint8_t SPI_RX_FILLER = 0xFFU;	///< Value to send as a filler while receiving multiple bytes

    //if no bytes to read, success
    if(!size)
        return ERR_SUCCESS;

    //assertions
    assert(_spiHandle);
    assert(value);

    //if register numbers above known, error
    if(firstRegister > ADXL_REGISTER_MAXNB)
        return (createErrorCode(READ_REGISTERS, 1, ERR_WARNING));

    //set timeout timer and enable SPI
    adxlSPITimer_ms = SPI_TIMEOUT_MS;
    LL_SPI_Enable(_spiHandle);
    uint8_t* iterator = value;

    //send the read request and ignore the first byte received (reply to the write request)
    LL_SPI_TransmitData8(_spiHandle, ADXL_READ | ADXL_MULTIPLE | firstRegister);
    while((!LL_SPI_IsActiveFlag_RXNE(_spiHandle)) && adxlSPITimer_ms);
    *iterator = LL_SPI_ReceiveData8(_spiHandle);

    //receive the bytes to read
    do{
        //send a filler byte to keep the SPI clock running, to receive the next byte
        LL_SPI_TransmitData8(_spiHandle, SPI_RX_FILLER);

        //wait for data to be available, and read it
        while((!LL_SPI_IsActiveFlag_RXNE(_spiHandle)) && adxlSPITimer_ms);
        *iterator = LL_SPI_ReceiveData8(_spiHandle);
        
        iterator++;
        size--;
    }while(size && adxlSPITimer_ms);

    //wait for transaction to be finished and clear Overrun flag
    while(LL_SPI_IsActiveFlag_BSY(_spiHandle) && adxlSPITimer_ms);
    LL_SPI_ClearFlag_OVR(_spiHandle);

    //disable SPI
    LL_SPI_Disable(_spiHandle);

    //if timeout, error
    if(!adxlSPITimer_ms)
        return (createErrorCode(READ_REGISTERS, 2, ERR_WARNING));

    return (ERR_SUCCESS);
}

/**
 * @brief Transpose a measurement to an angle in tenths of degrees with the Z axis
 *
 * @param axis Axis for which get the angle with the Z axis
 * @return Angle with the Z axis
 */
int16_t getAngleDegreesTenths(axis_e axis){
    static const float RADIANS_TO_DEGREES_TENTHS = 180.0f * 10.0f * (float)M_1_PI;

    if(!_latestValues[Z_AXIS])
        return (0);

    //compute the angle between Z axis and the requested one
    //	then transform radians to 0.1 degrees
    //	formula : degrees_tenths = (arctan(axis/Z) * 180° * 10) / PI
    float angle = atanf((float)(_latestValues[axis] + _zeroValues[axis]) / (float)_latestValues[Z_AXIS]);
    angle *= RADIANS_TO_DEGREES_TENTHS;
    return ((int16_t)angle);
}

/**
 * @brief Check the status of the ADXL Data Ready interrupt
 * 
 * @retval 0 Data is not ready yet
 * @retval 1 Data is ready
 */
static inline uint8_t isFIFOdataReady(){
    return !LL_GPIO_IsInputPinSet(ADXL_INT1_GPIO_Port, ADXL_INT1_Pin);
}

/**
 * @brief Reassemble a two's complement int16_t from two bytes
 * 
 * @note  Within the ADXL345's data registers, the LSB comes before the MSB
 * @param bytes		Array of two bytes to reassemble
 * @return int16_t	16 bit resulting number
 */
static inline int16_t twoComplement(const uint8_t bytes[2]){
    return (((int16_t)bytes[1] << 8) | (int16_t)bytes[0]);
}

/**
 * @brief Retrieve and average the values held in the ADXL FIFOs
 *
 * @param[out] values Integrated X, Y and Z axis values
 * @retval 0 Success
 * @retval 1 Error while retrieving values from the FIFO
 */
static errorCode_u integrateFIFO(int32_t values[]){
    uint8_t buffer[ADXL_NB_DATA_REGISTERS];
    uint8_t axis;

    //set the axis values to 0 before integrating
    values[X_AXIS] = values[Y_AXIS] = values[Z_AXIS] = 0;

    //for each of the samples to read
    for(uint8_t i = 0 ; i < ADXL_AVG_SAMPLES ; i++){
        //read all data registers for 1 sample
        _result = readRegisters(DATA_X0, buffer, ADXL_NB_DATA_REGISTERS);
        if(isError(_result)){
            _state = stError;
            return (pushErrorCode(_result, INTEGRATE, 1));
        }

        //add the measurements (formatted from a two's complement) to their final value buffer
        for(axis = 0 ; axis < NB_AXIS ; axis++)
            values[axis] += twoComplement(&buffer[axis << 1]);

        //wait for a while to make sure 5 us pass between two reads
        //	as stated in the datasheet, section "Retrieving data from the FIFO"
        volatile uint8_t tempo = 0x0FU;
        while(tempo--);
    }

    //divide the buffers to average out (Tested : compiler does divide negatives correctly)
    values[X_AXIS] >>= ADXL_AVG_SHIFT;
    values[Y_AXIS] >>= ADXL_AVG_SHIFT;
    values[Z_AXIS] >>= ADXL_AVG_SHIFT;

    return (ERR_SUCCESS);
}

/**
 * @brief Set the measurements in relative mode and zero down the values
 */
void ADXLzeroDown(){
    _zeroValues[X_AXIS] = -_latestValues[X_AXIS];
    _zeroValues[Y_AXIS] = -_latestValues[Y_AXIS];
}

/**
 * @brief Set the measurements in absolute mode (no zeroing compensation)
 */
void ADXLcancelZeroing(){
    for(uint8_t axis = 0 ; axis < NB_AXIS ; axis++)
        _zeroValues[axis] = 0;
}


/********************************************************************************************************************************************/
/********************************************************************************************************************************************/


/**
 * @brief Begin state of the state machine
 *
 * @retval 0 Success
 * @retval 1 Device ID invalid
 * @retval 2 Unable to read device ID
 */
static errorCode_u stStartup(){
    uint8_t deviceID = 0;

    //if 1s elapsed without reading the correct vendor ID, go error
    if(!adxlTimer_ms){
        _state = stError;
        return (createErrorCode(STARTUP, 1, ERR_CRITICAL));
    }

    //if unable to read device ID, error
    _result = readRegisters(DEVICE_ID, &deviceID, 1);
    if(isError(_result))
        return (pushErrorCode(_result, STARTUP, 2));

    //if invalid device ID, exit
    if(deviceID != ADXL_DEVICE_ID)
        return (ERR_SUCCESS);

    //reset timeout timer and get to next state
    _state = stConfiguring;
    return (ERR_SUCCESS);
}

/**
 * @brief State in which the registers of the ADXL are configured
 *
 * @retval 0 Success
 * @retval 1 Error while writing a register
 */
static errorCode_u stConfiguring(){
    static const uint8_t initialisationArray[NB_REG_INIT][2] = {
        {DATA_FORMAT,			DATA_FORMAT_DEFAULT},
        {BANDWIDTH_POWERMODE,	ADXL_POWER_NORMAL | ADXL_RATE_200HZ},
        {FIFO_CONTROL,			ADXL_MODE_BYPASS},		//clear the FIFOs first (blocks otherwise)
        {FIFO_CONTROL,			FIFO_CONTROL_DEFAULT},
        {POWER_CONTROL,			ADXL_MEASURE_MODE},		///
        {INTERRUPT_ENABLE,		ADXL_INT_WATERMARK},	///must come at the end
    };

    //write all registers values from the initialisation array
    for(uint8_t i = 0 ; i < NB_REG_INIT ; i++){
        _result = writeRegister(initialisationArray[i][0], initialisationArray[i][1]);
        if(isError(_result)){
            _state = stError;
            return (pushErrorCode(_result, INIT, 1));
        }
    }

    //reset the timer and get to next state
    adxlTimer_ms = INT_TIMEOUT_MS;
    _state = stMeasuringST_OFF;
    return (_result);
}

/**
 * @brief State in which the ADXL does some measurements with self-test OFF
 * @note p. 22, 31 and 32 of the datasheet
 *
 * @retval 0 Success
 * @retval 1 Timeout while waiting for measurements
 * @retval 2 Error while integrating the FIFOs
 * @retval 3 Error while enabling the self-testing mode
 * @retval 4 Error while clearing the FIFOs
 */
static errorCode_u stMeasuringST_OFF(){
    //if timeout, go error
    if(!adxlTimer_ms){
        _state = stError;
        return (createErrorCode(SELF_TESTING_OFF, 1, ERR_ERROR));
    }

    //if watermark interrupt not fired, exit
    if(!isFIFOdataReady())
        return (ERR_SUCCESS);

    //retrieve the integrated measurements (to be used with self-testing)
    _result = integrateFIFO(_latestValues);
    if(isError(_result)){
        _state = stError;
        return (pushErrorCode(_result, SELF_TESTING_OFF, 2));
    }

    //Enable the self-test
    _result = writeRegister(DATA_FORMAT, DATA_FORMAT_DEFAULT | ADXL_SELF_TEST);
    if(isError(_result)){
        _state = stError;
        return (pushErrorCode(_result, SELF_TESTING_OFF, 3));
    }

    //clear the FIFOs
    _result = writeRegister(FIFO_CONTROL, ADXL_MODE_BYPASS);
    if(isError(_result)){
        _state = stError;
        return (pushErrorCode(_result, SELF_TESTING_OFF, 4));
    }

    //set timer to wait for 25ms and get to next state
    static const uint8_t ST_WAIT_MS = 25U;  ///< Number of milliseconds to wait for self-testing to be operating
    adxlTimer_ms = ST_WAIT_MS;
    _state = stWaitingForSTenabled;
    return (ERR_SUCCESS);
}

/**
 * @brief State in which the ADXL waits for a while before restarting measurements
 *
 * @return 0 Success
 * @return 1 Error while re-enabling FIFOs
 */
static errorCode_u stWaitingForSTenabled(){
    //if timer not elapsed yet, exit
    if(!adxlTimer_ms)
        return (ERR_SUCCESS);

    //enable FIFOs
    _result = writeRegister(FIFO_CONTROL, FIFO_CONTROL_DEFAULT);
    if(isError(_result)){
        _state = stError;
        return (pushErrorCode(_result, SELF_TEST_WAIT, 1)); 	// @suppress("Avoid magic numbers")
    }

    //reset timer and get to next state
    adxlTimer_ms = INT_TIMEOUT_MS;
    _state = stMeasuringST_ON;
    return (ERR_SUCCESS);
}

/**
 * @brief State in which the ADXL measures while in self-test mode
 *
 * @retval 0 Success
 * @retval 1 Timeout while waiting for measurements
 * @retval 2 Error while integrating the FIFOs
 * @retval 3 Self-test values out of range
 * @retval 4 Error while resetting the data format
 */
static errorCode_u stMeasuringST_ON(){
    //ADXL Self-Test minimum and maximum delta values
    //	at 13-bits resolution, 16G range and 3.3V supply, according to the datasheet
    //	see Google Drive for calculations
    static const int16_t ST_MAXDELTAS[NB_AXIS][2] = {
        [X_AXIS] = {85, 949},
        [Y_AXIS] = {-949, -85},
        [Z_AXIS] = {118, 1294},
    };
    int32_t STdeltas[NB_AXIS];

    //if timeout, go error
    if(!adxlTimer_ms){
        _state = stError;
        return (createErrorCode(SELF_TESTING_ON, 1, ERR_ERROR));
    }

    //if watermark interrupt not fired, exit
    if(!isFIFOdataReady())
        return (ERR_SUCCESS);

    //integrate the FIFOs
    _result = integrateFIFO(STdeltas);
    if(isError(_result)){
        _state = stError;
        return (pushErrorCode(_result, SELF_TESTING_ON, 2));
    }

    //compute the self-test deltas
    STdeltas[X_AXIS] -= _latestValues[X_AXIS];
    STdeltas[Y_AXIS] -= _latestValues[Y_AXIS];
    STdeltas[Z_AXIS] -= _latestValues[Z_AXIS];

    //if self-test values out of range, error
    if((STdeltas[X_AXIS] <= ST_MAXDELTAS[X_AXIS][0]) || (STdeltas[X_AXIS] >= ST_MAXDELTAS[X_AXIS][1])
        || (STdeltas[Y_AXIS] <= ST_MAXDELTAS[Y_AXIS][0]) || (STdeltas[Y_AXIS] >= ST_MAXDELTAS[Y_AXIS][1])
        || (STdeltas[Z_AXIS] <= ST_MAXDELTAS[Z_AXIS][0]) || (STdeltas[Z_AXIS] >= ST_MAXDELTAS[Z_AXIS][1]))
    {
        _state = stError;
        return (pushErrorCode(_result, SELF_TESTING_ON, 3));
    }

    //reset the data format
    _result = writeRegister(DATA_FORMAT, DATA_FORMAT_DEFAULT);
    if(isError(_result)){
        _state = stError;
        return (pushErrorCode(_result, SELF_TESTING_ON, 4));
    }

    //reset timer and get to next state
    adxlTimer_ms = INT_TIMEOUT_MS;
    _state = stMeasuring;
    return (ERR_SUCCESS);
}

/**
 * @brief State in which the ADXL measures accelerations
 *
 * @retval 0 Success
 * @retval 1 Timeout occurred while waiting for watermark interrupt
 * @retval 2 Error occurred while integrating the FIFOs
 */
static errorCode_u stMeasuring(){
    //if timeout, go error
    if(!adxlTimer_ms){
        _state = stError;
        return (createErrorCode(MEASURE, 1, ERR_ERROR));
    }

    //if watermark interrupt not fired, exit
    if(!isFIFOdataReady())
        return (ERR_SUCCESS);

    //reset flags
    adxlTimer_ms = INT_TIMEOUT_MS;

    //integrate the FIFOs
    _result = integrateFIFO(_latestValues);
    if(isError(_result)){
        _state = stError;
        return (pushErrorCode(_result, MEASURE, 2));
    }

    _measurementsUpdated = 1;
    return (ERR_SUCCESS);
}

/**
 * @brief State in which the ADXL stays in an error state forever
 *
 * @return Success
 */
static errorCode_u stError(){
    return (ERR_SUCCESS);
}
