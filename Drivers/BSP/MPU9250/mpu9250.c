#include "mpu9250.h"
#include "stm32f1xx_hal.h"
#include "math.h"

static float invSqrt(float x);

//#define twoKiDef        (2.0f * 0.5f)   // 2 * integral gain
#define twoKiDef        (2.0f * 0.0f)   // 2 * integral gain
#define twoKpDef        (2.0f * 0.5f)   // 2 * proportional gain

float twoKp = twoKpDef;                                             // 2 * proportional gain (Kp)
float twoKi = twoKiDef;                                             // 2 * integral gain (Ki)
float q0 = 1.0f, q1 = 0.0f, q2 = 0.0f, q3 = 0.0f;     // quaternion of sensor frame relative to auxiliary frame
float integralFBx = 0.0f, integralFBy = 0.0f, integralFBz = 0.0f;  // integral error terms scaled by Ki
float acc_divider;
float gyro_divider;
float gy[3];
float ac[3];
float ang[3];
uint8_t acc_bias_data_save[6];

/**
 * @brief  Initializes the MPU communication.
 * @param  sample_rate_div  (1), GY_low_pass_filter (0x01), AC_low_pass_filter (0x01)
 * @retval ok: 0x00 error: 0x01
 */
uint8_t BSP_MPU_Init(uint8_t sample_rate_div, uint8_t GY_low_pass_filter, uint8_t AC_low_pass_filter)
{

    uint8_t i = 0;
//    uint8_t acc_bias_data[6] = { 0, 0, 0, 0, 0, 0 };

    uint8_t MPU_Init_Data[MPU_InitRegNum][2] =
    {
        { 0x80, MPUREG_PWR_MGMT_1 },     // Reset Device
        { 0x01, MPUREG_PWR_MGMT_1 },     // Clock Source
        { 0x00, MPUREG_PWR_MGMT_2 },     // Enable Acc & Gyro
        { GY_low_pass_filter, MPUREG_CONFIG },   // Use DLPF set Gyroscope bandwidth and temperature 0=250Hz
        { sample_rate_div, MPUREG_SMPLRT_DIV }, // sample_rate_div RATE=internal_RATE / (div + 1)
        { 0x18, MPUREG_GYRO_CONFIG },    // +-2000dps, Bit 0-1 unset (FCHOICE_B) enables LPF (FCHOICE set)
        { 0x08, MPUREG_ACCEL_CONFIG },   // +-4G
        { AC_low_pass_filter, MPUREG_ACCEL_CONFIG_2 }, // Set Acc Data Rates, Enable Acc LPF  ACCEL_FCHOICE_B (Bit3), LPF Bit 0-2
                                                       // Bit 3 = 0 (ACCEL_FCHOICE = 1) and Bit 0 - 2 = 0 ->  LPF enabled, BW 460 Hz, Rate 1kHz
        { 0x30, MPUREG_INT_PIN_CFG },    //
        { 0x30, MPUREG_USER_CTRL },       // I2C Master mode
        { 0x0D, MPUREG_I2C_MST_CTRL }, //  I2C configuration multi-master  IIC 400KHz
        { AK8963_I2C_ADDR, MPUREG_I2C_SLV0_ADDR },  //Set the I2C slave addres of AK8963 and set for write.
        { AK8963_CNTL2, MPUREG_I2C_SLV0_REG }, //I2C slave 0 register address from where to begin data transfer
        { 0x01, MPUREG_I2C_SLV0_DO }, // Reset AK8963
        { 0x81, MPUREG_I2C_SLV0_CTRL },  //Enable I2C and set 1 byte
        { AK8963_CNTL1, MPUREG_I2C_SLV0_REG }, //I2C slave 0 register address from where to begin data transfer
        { 0x12, MPUREG_I2C_SLV0_DO }, // Register value to continuous measurement in 16bit
        { 0x81, MPUREG_I2C_SLV0_CTRL },  //Enable I2C and set 1 byte
        { 0x01, MPUREG_INT_ENABLE }  //Enables Data ready Signal on Int pin
    };

    /* Configure IO functionalities for MPU pin */
    MPU_IO_Init();

    for (i = 0; i < MPU_InitRegNum; i++)
    {
        MPU_IO_WriteReadReg(MPU_Init_Data[i][1], MPU_Init_Data[i][0]);
        HAL_Delay(1);  //I2C must slow down the write speed, otherwise it won't work
    }

    BSP_MPU_set_acc_scale(BITS_FS_4G);
    BSP_MPU_set_gyro_scale(BITS_FS_1000DPS);

    // save values of acc cancellation registers
    BSP_MPU_ReadRegs(MPUREG_XA_OFFSET_H, (uint8_t*) &acc_bias_data_save[0], 2);
    BSP_MPU_ReadRegs(MPUREG_YA_OFFSET_H, (uint8_t*) &acc_bias_data_save[2], 2);
    BSP_MPU_ReadRegs(MPUREG_ZA_OFFSET_H, (uint8_t*) &acc_bias_data_save[4], 2);

    return 0;

}

uint8_t BSP_MPU_Whoami()
{
    uint8_t response;

    response = MPU_IO_WriteReadReg(MPUREG_WHOAMI | READ_FLAG, 0x00);

    return response;
}

uint8_t BSP_MPU_set_acc_scale(uint8_t scale)
{
    uint8_t temp_scale;
    MPU_IO_WriteReadReg(MPUREG_ACCEL_CONFIG, scale);

    switch (scale)
    {
    case BITS_FS_2G:
        acc_divider = 16384;
        break;
    case BITS_FS_4G:
        acc_divider = 8192;
        break;
    case BITS_FS_8G:
        acc_divider = 4096;
        break;
    case BITS_FS_16G:
        acc_divider = 2048;
        break;
    }
    temp_scale = MPU_IO_WriteReadReg(MPUREG_ACCEL_CONFIG | READ_FLAG, 0x00);

    switch (temp_scale & 0x18)
    {
    case BITS_FS_2G:
        temp_scale = 2;
        break;
    case BITS_FS_4G:
        temp_scale = 4;
        break;
    case BITS_FS_8G:
        temp_scale = 8;
        break;
    case BITS_FS_16G:
        temp_scale = 16;
        break;
    }
    return temp_scale;
}

uint16_t BSP_MPU_set_gyro_scale(uint8_t scale)
{
    uint16_t temp_scale;
    MPU_IO_WriteReadReg(MPUREG_GYRO_CONFIG, scale);
    switch (scale)
    {
    case BITS_FS_250DPS:
        gyro_divider = 131;
        break;
    case BITS_FS_500DPS:
        gyro_divider = 65.5;
        break;
    case BITS_FS_1000DPS:
        gyro_divider = 32.8;
        break;
    case BITS_FS_2000DPS:
        gyro_divider = 16.4;
        break;
    }
    temp_scale = MPU_IO_WriteReadReg(MPUREG_GYRO_CONFIG | READ_FLAG, 0x00);
    switch (temp_scale & 0x18)
    {
    case BITS_FS_250DPS:
        temp_scale = 250;
        break;
    case BITS_FS_500DPS:
        temp_scale = 500;
        break;
    case BITS_FS_1000DPS:
        temp_scale = 1000;
        break;
    case BITS_FS_2000DPS:
        temp_scale = 2000;
        break;
    }
    return temp_scale;
}

void BSP_MPU_ReadRegs(uint8_t ReadAddr, uint8_t *ReadBuf, uint8_t Bytes)
{
    uint8_t i = 0;

    MPU_IO_CSState(0);

    MPU_IO_WriteByte(ReadAddr | READ_FLAG);
    for (i = 0; i < Bytes; i++)
        ReadBuf[i] = MPU_IO_WriteByte(0x00);

    MPU_IO_CSState(1);

    // HAL_Delay(50);
}

void BSP_MPU_WriteRegs(uint8_t WriteAddr, uint8_t *WriteBuf, uint8_t Bytes)
{
    uint8_t i = 0;

    MPU_IO_CSState(0);

    MPU_IO_WriteByte(WriteAddr);
    for (i = 0; i < Bytes; i++)
    {
        MPU_IO_WriteByte(WriteBuf[i]);
    }

    MPU_IO_CSState(1);
}

void BSP_MPU_read_rot()
{
    uint8_t response[6];
    int16_t bit_data;
    float data;
    int i;
    /* undocumented reading all gyro values after adressing GYRO_XOUT_H */
    BSP_MPU_ReadRegs(MPUREG_GYRO_XOUT_H, response, 6);
    for (i = 0; i < 3; i++)
    {
        bit_data = ((int16_t) response[i * 2] << 8) | response[i * 2 + 1];
        data = (float) bit_data;
        gy[i] = data / gyro_divider;
    }
}

void BSP_MPU_read_acc()
{
    uint8_t response[6];
    int16_t bit_data;
    float data;
    int i;
    /* undocumented reading all gyro values after adressing ACCEL_XOUT_H */
    BSP_MPU_ReadRegs(MPUREG_ACCEL_XOUT_H, response, 6);
    for (i = 0; i < 3; i++)
    {
        bit_data = ((int16_t) response[i * 2] << 8) | response[i * 2 + 1];
        data = (float) bit_data;
        ac[i] = data / acc_divider;
    }
}

static float invSqrt(float x)
{
    union
    {
        int i;
        float f;
    } u;

    float x2;
    const float threehalfs = 1.5F;

    x2 = x * 0.5F;
    u.f = x;
    u.i = 0x5f3759df - (u.i >> 1); // You are not supposed to understand this
    u.f = u.f * (threehalfs - (x2 * u.f * u.f));
    // u.f = u.f * ( threehalfs - ( x2 * u.f * u.f ) );

    return u.f;
}

void BSP_MPU_updateIMU(float ax, float ay, float az, float gx, float gy, float gz, float dt)
{
    float recipNorm;
    float halfvx, halfvy, halfvz;
    float halfex, halfey, halfez;
    float qa, qb, qc;

    dt /= 1000.0f;

    // degrees to radiant conversion
    gx *= 0.0174533f;
    gy *= 0.0174533f;
    gz *= 0.0174533f;

    // Compute feedback only if accelerometer measurement valid (avoids NaN in accelerometer normalisation)
    if (!((ax == 0.0f) && (ay == 0.0f) && (az == 0.0f)))
    {

        // Normalize accelerometer measurement
        recipNorm = invSqrt(ax * ax + ay * ay + az * az);
        ax *= recipNorm;
        ay *= recipNorm;
        az *= recipNorm;

        // Estimated direction of gravity and vector perpendicular to magnetic flux
        halfvx = q1 * q3 - q0 * q2;
        halfvy = q0 * q1 + q2 * q3;
        halfvz = q0 * q0 - 0.5f + q3 * q3;

        // Error is sum of cross product between estimated and measured direction of gravity
        halfex = (ay * halfvz - az * halfvy);
        halfey = (az * halfvx - ax * halfvz);
        halfez = (ax * halfvy - ay * halfvx);

        // Compute and apply integral feedback if enabled
        if (twoKi > 0.0f)
        {
            integralFBx += twoKi * halfex * dt;	// integral error scaled by Ki
            integralFBy += twoKi * halfey * dt;
            integralFBz += twoKi * halfez * dt;
            gx += integralFBx;	// apply integral feedback
            gy += integralFBy;
            gz += integralFBz;
        }
        else
        {
            integralFBx = 0.0f;	// prevent integral windup
            integralFBy = 0.0f;
            integralFBz = 0.0f;
        }

        // Apply proportional feedback
        gx += twoKp * halfex;
        gy += twoKp * halfey;
        gz += twoKp * halfez;
    }

    // Integrate rate of change of quaternion
    gx *= (0.5f * dt);		// pre-multiply common factors
    gy *= (0.5f * dt);
    gz *= (0.5f * dt);
    qa = q0;
    qb = q1;
    qc = q2;
    q0 += (-qb * gx - qc * gy - q3 * gz);
    q1 += (qa * gx + qc * gz - q3 * gy);
    q2 += (qa * gy - qb * gz + q3 * gx);
    q3 += (qa * gz + qb * gy - qc * gx);

    // Normalize quaternion
    recipNorm = invSqrt(q0 * q0 + q1 * q1 + q2 * q2 + q3 * q3);
    q0 *= recipNorm;
    q1 *= recipNorm;
    q2 *= recipNorm;
    q3 *= recipNorm;
}

/*
 // aSin2 function
 // ArcSin2: a more precise asin. Y=opposite leg, R=radius/hypotenuse.
 // Of course if R is 0, no line exists, so it will return 0. Note that this assumes you put the asin code in your script.

 final static function float ASin2(float Y,float Rad)
 {
 local float tempang;

 if(Rad==0)
 return 0; //technically impossible (no hypotenuse = nothing)
 tempang=ASin(Y/Rad);

 if (Rad<0)
 tempang=pi-tempang;  //lower quads

 return tempang;
 }
 */

void BSP_MPU_getEuler(float* roll, float* pitch, float* yaw)
{
    *roll = atan2f(2 * (q0 * q1 + q2 * q3), 1 - 2 * (q1 * q1 + q2 * q2));
    *pitch = asinf(2 * (q0 * q2 - q3 * q1));
    *yaw = atan2f(2 * (q0 * q3 + q1 * q2), 1 - 2 * (q2 * q2 + q3 * q3));
}

void BSP_MPU_GyroCalibration(void)
{
    uint8_t response[6], divider;
    uint8_t i, j;
    int16_t offset_tmp[3] = { 0, 0, 0 };
    int32_t offset[3] = { 0, 0, 0 };
    uint8_t data[6] = { 0, 0, 0, 0, 0, 0 };

    for (j = 0; j < 200; j++)
    {
        BSP_MPU_ReadRegs(MPUREG_GYRO_XOUT_H, response, 6);
        for (i = x; i <= z; i++)
        {
            offset_tmp[i] = ((int16_t) response[i * 2] << 8) | response[i * 2 + 1];
            offset[i] -= (int32_t) offset_tmp[i];
        }
        HAL_Delay(5);
    }

    divider = MPU_IO_WriteReadReg(MPUREG_GYRO_CONFIG | READ_FLAG, 0x00);
    divider &= 0x18;

    switch (divider)
    {

    case BITS_FS_250DPS:
        divider = 8;
        break;
    case BITS_FS_500DPS:
        divider = 4;
        break;
    case BITS_FS_1000DPS:
        divider = 2;
        break;
    case BITS_FS_2000DPS:
        divider = 1;
        break;
    }

    // offset register referred to 1000 DPS
    // From Invensense Motion Driver, avoid round errors
    offset[x] = (int32_t) ( ( (int64_t) offset[x] ) / divider / 100 );
    offset[y] = (int32_t) ( ( (int64_t) offset[y] ) / divider / 100 );
    offset[z] = (int32_t) ( ( (int64_t) offset[z] ) / divider / 100 );

    // This works too
    //offset[x] /= (int32_t) (100 * divider);
    //offset[y] /= (int32_t) (100 * divider);
    //offset[z] /= (int32_t) (100 * divider);

    // swapped bytes
    data[0] = (offset[x] >> 8) & 0xFF;
    data[1] = offset[x] & 0xFF;
    data[2] = (offset[y] >> 8) & 0xFF;
    data[3] = offset[y] & 0xFF;
    data[4] = (offset[z] >> 8) & 0xFF;
    data[5] = offset[z] & 0xFF;

    BSP_MPU_WriteRegs(MPUREG_XG_OFFS_USRH, (uint8_t*) &data[0], 2);
    BSP_MPU_WriteRegs(MPUREG_YG_OFFS_USRH, (uint8_t*) &data[2], 2);
    BSP_MPU_WriteRegs(MPUREG_ZG_OFFS_USRH, (uint8_t*) &data[4], 2);
}

uint8_t BSP_Acc_Collect_Error_cal(int32_t *acc_offset, uint8_t count)
{
    uint8_t response[6], divider;
    int16_t acc_offset_tmp[3];
    uint8_t i;

    BSP_MPU_ReadRegs(MPUREG_ACCEL_XOUT_H, response, 6);
    for (i = x; i <= z; i++)
    {
        acc_offset_tmp[i] = ((int16_t) response[i * 2] << 8) | response[i * 2 + 1];
        acc_offset[i] += (int32_t) acc_offset_tmp[i];
    }

    if (count == 0) // finally process average offsets
    {
        divider = MPU_IO_WriteReadReg(MPUREG_ACCEL_CONFIG | READ_FLAG, 0x00);
        divider &= 0x18;

        switch (divider)
        {
        case BITS_FS_2G:
            divider = 8;
            break;
        case BITS_FS_4G:
            divider = 4;
            break;
        case BITS_FS_8G:
            divider = 2;
            break;
        case BITS_FS_16G:
            divider = 1;
            break;
        }

        // offset register referred to 16G (against example from Invensense which says 8G)
        // from Invensense Motion Driver, avoid round errors
        acc_offset[x] = (int32_t) ( ( (int64_t) acc_offset[x] ) / divider / ACC_ERR_COLLECT_COUNT );
        acc_offset[y] = (int32_t) ( ( (int64_t) acc_offset[y] ) / divider / ACC_ERR_COLLECT_COUNT );
        acc_offset[z] = (int32_t) ( ( (int64_t) acc_offset[z] ) / divider / ACC_ERR_COLLECT_COUNT );

        // One axis must be very near to vertical, subtract value for 1G from it
        for (i = 0; i <= 2; i++)
        {
            if (fabsf(acc_offset[i]) > 1000)
            {
                if (acc_offset[i] > 1000)
                {
                    acc_offset[i] -= 2048;
                }
                else if (acc_offset[i] < 1000)
                {
                    acc_offset[i] += 2048;
                }
            }
        }

        BSP_MPU_AccCalibration(acc_offset);

        return 0;
    }

    return 1;
}

void BSP_Revert_Acc_Cancel()
{
    // restore saved cancellation registers
    BSP_MPU_WriteRegs(MPUREG_XA_OFFSET_H, (uint8_t*) &acc_bias_data_save[0], 2);
    BSP_MPU_WriteRegs(MPUREG_YA_OFFSET_H, (uint8_t*) &acc_bias_data_save[2], 2);
    BSP_MPU_WriteRegs(MPUREG_ZA_OFFSET_H, (uint8_t*) &acc_bias_data_save[4], 2);
}

void BSP_Get_MPU_Acc_Offset(int32_t *acc_offset)
{
    uint8_t i, j;
    uint8_t response[6], divider;
    int16_t acc_offset_tmp[3];

    // restore saved cancellation registers
    BSP_MPU_WriteRegs(MPUREG_XA_OFFSET_H, (uint8_t*) &acc_bias_data_save[0], 2);
    BSP_MPU_WriteRegs(MPUREG_YA_OFFSET_H, (uint8_t*) &acc_bias_data_save[2], 2);
    BSP_MPU_WriteRegs(MPUREG_ZA_OFFSET_H, (uint8_t*) &acc_bias_data_save[4], 2);

    for (j = 0; j < 200; j++)
    {
        BSP_MPU_ReadRegs(MPUREG_ACCEL_XOUT_H, response, 6);
        for (i = x; i <= z; i++)
        {
            acc_offset_tmp[i] = ((int16_t) response[i * 2] << 8) | response[i * 2 + 1];
            acc_offset[i] += (int32_t) acc_offset_tmp[i];
        }

        HAL_Delay(5);
    }

    divider = MPU_IO_WriteReadReg(MPUREG_ACCEL_CONFIG | READ_FLAG, 0x00);
    divider &= 0x18;

    switch (divider)
    {
    case BITS_FS_2G:
        divider = 8;
        break;
    case BITS_FS_4G:
        divider = 4;
        break;
    case BITS_FS_8G:
        divider = 2;
        break;
    case BITS_FS_16G:
        divider = 1;
        break;
    }

    // offset register referred to 16G (against example from Invensense which says 8G)
    // from Invensense Motion Driver, avoid round errors
    acc_offset[x] = (int32_t) ( ( (int64_t) acc_offset[x] ) / divider / 200 );
    acc_offset[y] = (int32_t) ( ( (int64_t) acc_offset[y] ) / divider / 200 );
    acc_offset[z] = (int32_t) ( ( (int64_t) acc_offset[z] ) / divider / 200 );

    /*
     * this works too
     acc_offset[x] /= (int32_t) (200 * divider);
     acc_offset[y] /= (int32_t) (200 * divider);
     acc_offset[z] /= (int32_t) (200 * divider);
     */

    // One axis must be very near to vertical, subtract value for 1G from it
    for (i = 0; i <= 2; i++)
    {
        if (fabsf(acc_offset[i]) > 1000)
        {
            if (acc_offset[i] > 1000)
            {
                acc_offset[i] -= 2048;
            }
            else if (acc_offset[i] < 1000)
            {
                acc_offset[i] += 2048;
            }
        }
    }
}

void BSP_MPU_AccCalibration(int32_t *acc_offset)
{
    int16_t bias[3] = { 0, 0, 0 };
    uint8_t data[6] = { 0, 0, 0, 0, 0, 0 };

    bias[0] = ((int16_t) acc_bias_data_save[0] << 8) | acc_bias_data_save[1];
    bias[1] = ((int16_t) acc_bias_data_save[2] << 8) | acc_bias_data_save[3];
    bias[2] = ((int16_t) acc_bias_data_save[4] << 8) | acc_bias_data_save[5];

    bias[0] -= (acc_offset[0] & ~1);
    bias[1] -= (acc_offset[1] & ~1);
    bias[2] -= (acc_offset[2] & ~1);

    // swapped bytes
    data[0] = (bias[x] >> 8) & 0xFF;
    data[1] = bias[x] & 0xFF;
    data[2] = (bias[y] >> 8) & 0xFF;
    data[3] = bias[y] & 0xFF;
    data[4] = (bias[z] >> 8) & 0xFF;
    data[5] = bias[z] & 0xFF;

    BSP_MPU_WriteRegs(MPUREG_XA_OFFSET_H, (uint8_t*) &data[0], 2);
    BSP_MPU_WriteRegs(MPUREG_YA_OFFSET_H, (uint8_t*) &data[2], 2);
    BSP_MPU_WriteRegs(MPUREG_ZA_OFFSET_H, (uint8_t*) &data[4], 2);
}
