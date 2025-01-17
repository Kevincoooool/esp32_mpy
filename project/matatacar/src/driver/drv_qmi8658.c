#include "drv_qmi8658.h"
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include "py/objstr.h"
#include "py/runtime.h"
#include "py/mphal.h"
//#include "esp32/rom/ets_sys.h"

#include "esp_log.h"
#define   TAG                         ("GYRO")

#define QMI8658_SLAVE_ADDR_L			0x6a
#define QMI8658_SLAVE_ADDR_H			0x6a
#define I2C_MASTER_NUM I2C_NUM_1
double gyro_x_calib = 4.246f,gyro_y_calib = -27.252f,gyro_z_calib = -5.940f;
double acc_x_calib,acc_y_calib,acc_z_calib;
enum
{
    AXIS_X = 0,
    AXIS_Y = 1,
    AXIS_Z = 2,

    AXIS_TOTAL
};

typedef struct 
{
    short 				sign[AXIS_TOTAL];
    unsigned short 		map[AXIS_TOTAL];
}qst_imu_layout;

static unsigned short acc_lsb_div = 0;
static unsigned short gyro_lsb_div = 0;

static struct Qmi8658Config qmi8658_config;
static unsigned char qmi8658_slave_addr = QMI8658_SLAVE_ADDR_L;

typedef struct
{ 
  int16_t threshold_value_high;
  int16_t threshold_value_low;
  uint8_t event_occured_flag;
}gyro_event_manager_t;

gyro_event_manager_t gyro_event_info[EVENT_MAX];

static mt_err_t mt_esp32_gyro_event_listening_t(float ax, float ay, float az);

void delayMs(unsigned int ms)
{
    int i;
    for(i = 0;i < ms;i++)
    {
        ets_delay_us(1000);
    }
}
unsigned char Qmi8658_write_reg(unsigned char reg, unsigned char value)
{
    i2c_master_write_reg(I2C_MASTER_NUM, qmi8658_slave_addr ,reg, value);
    return 0;
}

unsigned char Qmi8658_read_reg(unsigned char reg, unsigned char* buf, unsigned short len)
{
    i2c_master_read_mem(I2C_MASTER_NUM, qmi8658_slave_addr, reg, buf,len);
    return 0;
}

void Qmi8658_enableSensors(unsigned char enableFlags)
{
    if(enableFlags & QMI8658_CONFIG_AE_ENABLE)
    {
        enableFlags |= QMI8658_CTRL7_ACC_ENABLE | QMI8658_CTRL7_GYR_ENABLE;
    }

    Qmi8658_write_reg(Qmi8658Register_Ctrl7, enableFlags | 0x80);
}

void Qmi8658_config_acc(enum Qmi8658_AccRange range, enum Qmi8658_AccOdr odr, enum Qmi8658_LpfConfig lpfEnable, enum Qmi8658_StConfig stEnable)
{
    unsigned char ctl_dada;

    switch(range)
    {
        case Qmi8658AccRange_2g:			acc_lsb_div = (1<<14);			break;
        case Qmi8658AccRange_4g:			acc_lsb_div = (1<<13);			break;
        case Qmi8658AccRange_8g:			acc_lsb_div = (1<<12);			break;
        case Qmi8658AccRange_16g:			acc_lsb_div = (1<<11);			break;
        default: 			
            range = Qmi8658AccRange_8g;		acc_lsb_div = (1<<12);
    }
    if(stEnable == Qmi8658St_Enable)
        ctl_dada = (unsigned char)range|(unsigned char)odr|0x80;
    else
        ctl_dada = (unsigned char)range|(unsigned char)odr;
        
    Qmi8658_write_reg(Qmi8658Register_Ctrl2, ctl_dada);
// set LPF & HPF
    Qmi8658_read_reg(Qmi8658Register_Ctrl5, &ctl_dada,1);
    ctl_dada &= 0xf0;
    if(lpfEnable == Qmi8658Lpf_Enable)
    {
        ctl_dada |= A_LSP_MODE_3;
        ctl_dada |= 0x01;
    }
    else
    {
        ctl_dada &= ~0x01;
    }
    ctl_dada = 0x00;
    Qmi8658_write_reg(Qmi8658Register_Ctrl5,ctl_dada);
// set LPF & HPF
}

void Qmi8658_config_gyro(enum Qmi8658_GyrRange range, enum Qmi8658_GyrOdr odr, enum Qmi8658_LpfConfig lpfEnable, enum Qmi8658_StConfig stEnable)
{
    // Set the CTRL3 register to configure dynamic range and ODR
    unsigned char ctl_dada; 

    // Store the scale factor for use when processing raw data
    switch(range)
    {
        case Qmi8658GyrRange_32dps:			gyro_lsb_div = 1024;		break;
        case Qmi8658GyrRange_64dps:			gyro_lsb_div = 512;			break;
        case Qmi8658GyrRange_128dps:		gyro_lsb_div = 256;			break;
        case Qmi8658GyrRange_256dps:		gyro_lsb_div = 128;			break;
        case Qmi8658GyrRange_512dps:		gyro_lsb_div = 64;			break;
        case Qmi8658GyrRange_1024dps:		gyro_lsb_div = 32;			break;
        case Qmi8658GyrRange_2048dps:		gyro_lsb_div = 16;			break;
        //case Qmi8658GyrRange_4096dps:		gyro_lsb_div = 8;			break;
        default: 		range = Qmi8658GyrRange_512dps;			gyro_lsb_div = 64;		break;
    }

    if(stEnable == Qmi8658St_Enable)
        ctl_dada = (unsigned char)range|(unsigned char)odr|0x80;
    else
        ctl_dada = (unsigned char)range | (unsigned char)odr;
    Qmi8658_write_reg(Qmi8658Register_Ctrl3, ctl_dada);

// Conversion from degrees/s to rad/s if necessary
// set LPF & HPF
    Qmi8658_read_reg(Qmi8658Register_Ctrl5, &ctl_dada,1);
    ctl_dada &= 0x0f;
    if(lpfEnable == Qmi8658Lpf_Enable)
    {
        ctl_dada |= G_LSP_MODE_3;
        ctl_dada |= 0x10;
    }
    else
    {
        ctl_dada &= ~0x10;
    }
    ctl_dada = 0x00;
    Qmi8658_write_reg(Qmi8658Register_Ctrl5,ctl_dada);
// set LPF & HPF
}




short gyroraw[3];
short accelraw[3];

short accelraw_last[3];
short gyroraw_last[3];
short gyroraw_test[3];

short acc_test[3];

void Qmi8658_read_xyz(void)
{
    //unsigned char	buf_reg[12];
    unsigned char  Sensor_temp[14];
    unsigned char  status;
    union{
        unsigned char ch[2];
        short	ss;
    }c2s;

    Qmi8658_read_reg(Qmi8658Register_StatusInt, &status, 1);

    if(status == 0x01)
    {
        ets_delay_us(6);
    }
    if((status == 0x01) || (status == 0x03))
    {
        Qmi8658_read_reg(Qmi8658Register_Ax_L, Sensor_temp, 12); 	// 0x19, 25
        c2s.ch[0] = Sensor_temp[6];	c2s.ch[1] = Sensor_temp[7];	gyroraw[1]  =  c2s.ss;
        c2s.ch[0] = Sensor_temp[8];	c2s.ch[1] = Sensor_temp[9];	gyroraw[0]  =  c2s.ss;
        c2s.ch[0] = Sensor_temp[10];c2s.ch[1] = Sensor_temp[11];gyroraw[2]  =  c2s.ss;
        
        c2s.ch[0] = Sensor_temp[0];	c2s.ch[1] = Sensor_temp[1];	accelraw[1] =  c2s.ss;
        c2s.ch[0] = Sensor_temp[2];	c2s.ch[1] = Sensor_temp[3];	accelraw[0] =  c2s.ss;
        c2s.ch[0] = Sensor_temp[4];	c2s.ch[1] = Sensor_temp[5];	accelraw[2] =  c2s.ss;
        
        accelraw_last[0] = accelraw[0];
        accelraw_last[1] = accelraw[1];
        accelraw_last[2] = accelraw[2];

        gyroraw_last[0] = gyroraw[0];
        gyroraw_last[1] = gyroraw[1];
        gyroraw_last[2] = gyroraw[2];
    }
    else
    {
        accelraw[0] = accelraw_last[0];
        accelraw[1] = accelraw_last[1];
        accelraw[2] = accelraw_last[2];
        
        gyroraw[0] = gyroraw_last[0];
        gyroraw[1] = gyroraw_last[1];
        gyroraw[2] = gyroraw_last[2];
    }

    acc_test[1] = accelraw[1];
    acc_test[0] = accelraw[0];
    acc_test[2] = accelraw[2];

    
    gyroraw_test[1] = gyroraw[1];
    gyroraw_test[0] = gyroraw[0];
    gyroraw_test[2] = gyroraw[2];

    accelraw[1] -= acc_x_calib;
    accelraw[0] -= acc_y_calib;
    accelraw[2] -= acc_z_calib;
    //gyro_x_y_z_calibration();
    gyroraw[1] = gyroraw_test[1] - gyro_x_calib;
    gyroraw[0] = gyroraw_test[0] - gyro_y_calib;
    gyroraw[2] = gyroraw_test[2] - gyro_z_calib;
    //gyro_x_calibration,gyro_y_calibration,gyro_z_calibration



}
void qmi8658_calibration(void)
{
    /*gyro_x_y_z_calibration*/

    static double gyro_x_calibration, gyro_y_calibration,gyro_z_calibration, acc_x_calibration, acc_y_calibration, acc_z_calibration;
    static int gyro_x_y_z_calibration_number = 0;
    printf("calibration start\n");
    while(1)
    {
        mp_hal_delay_ms(10);
        Qmi8658_read_xyz();
        gyro_x_y_z_calibration_number++;
        gyro_x_calibration += gyroraw_test[1];
        gyro_y_calibration += gyroraw_test[0];
        gyro_z_calibration += gyroraw_test[2];

        acc_x_calibration  += acc_test[1];
        acc_y_calibration  += acc_test[0];
        acc_z_calibration  += acc_test[2];

        if(gyro_x_y_z_calibration_number > 499)
        {
            gyro_x_y_z_calibration_number= 0;
            gyro_x_calibration = gyro_x_calibration/500;
            gyro_y_calibration = gyro_y_calibration/500;
            gyro_z_calibration = gyro_z_calibration/500;
            acc_x_calibration = acc_x_calibration/500;
            acc_y_calibration = acc_y_calibration/500;
            acc_z_calibration = acc_z_calibration/500;
            printf("gyro_x_calibration = %f\n",gyro_x_calibration);//73.708
            printf("gyro_y_calibration = %f\n",gyro_y_calibration);//16.228
            printf("gyro_z_calibration = %f\n",gyro_z_calibration);//27.320
            acc_x_calib = acc_x_calibration;
            acc_y_calib = acc_y_calibration;
            acc_z_calib = acc_z_calibration - 2048;
            gyro_x_calib = gyro_x_calibration;
            gyro_y_calib = gyro_y_calibration;
            gyro_z_calib = gyro_z_calibration;

            gyro_x_calibration = 0;
            gyro_y_calibration = 0;
            gyro_z_calibration = 0;
            acc_x_calibration = 0;
            acc_y_calibration = 0;
            acc_z_calibration = 0;
            break;
        }
    }
    printf("calibration over\n");

}
float gyro_avg_x,gyro_avg_y,gyro_avg_z;
void get_acc_gyro_angle(void)
{
    Qmi8658_read_xyz();
    acc_x = (float)accelraw[1]*ONE_G/2048;
    acc_y = (float)accelraw[0]*ONE_G/2048;
    acc_z = (float)accelraw[2]*ONE_G/2048;

    acc_angle_x = (atan2(acc_x, sqrt( pow(acc_y, 2) + pow(acc_z, 2) ) ) * 180.0 / M_PI);
    acc_angle_y = (atan2(acc_y, sqrt( pow(acc_x, 2) + pow(acc_z, 2) ) ) * 180.0 / M_PI);  
    acc_angle_z = (atan(acc_z/sqrt(pow(acc_x,2)+pow(acc_y,2)))*180.0/M_PI);

    gyro_x = (float)gyroraw[1]/16;
    gyro_y = (float)gyroraw[0]/16;
    gyro_z = (float)gyroraw[2]/16;

}
void reset_rotion(void)
{
    acc_x_calib = acc_test[1];
    acc_y_calib = acc_test[0];
    acc_z_calib = acc_test[2] - 2048 ;
    gyro_x_calib = gyroraw_test[1];
    gyro_y_calib = gyroraw_test[0];
    gyro_z_calib = gyroraw_test[2];
}
void Qmi8658_Config_apply(struct Qmi8658Config const* config)
{
    unsigned char fisSensors = config->inputSelection;

    if(config->inputSelection & QMI8658_CONFIG_ACC_ENABLE)
    {
        Qmi8658_config_acc(config->accRange, config->accOdr, Qmi8658Lpf_Disable, Qmi8658St_Disable);
    }
    
    if (config->inputSelection & QMI8658_CONFIG_GYR_ENABLE)
    {
        Qmi8658_config_gyro(config->gyrRange, config->gyrOdr, Qmi8658Lpf_Disable, Qmi8658St_Disable);
    }

    Qmi8658_enableSensors(fisSensors);	
}


unsigned char Qmi8658_init(void)
{
    unsigned char qmi8658_chip_id = 0x00;
     if(!is_i2c1_initialized())
    {
        i2c_master_init(I2C_MASTER_NUM);
    }
    while(1)
    {
        Qmi8658_read_reg(Qmi8658Register_WhoAmI, &qmi8658_chip_id, 1);
        delayMs(100);
        if(qmi8658_chip_id == 0x05)	break;
    }
    
    if(qmi8658_chip_id == 0x05)
    {
        Qmi8658_write_reg(Qmi8658Register_Ctrl1, 0x60);
        qmi8658_config.inputSelection = QMI8658_CONFIG_ACCGYR_ENABLE;//QMI8658_CONFIG_ACCGYR_ENABLE;
        qmi8658_config.accRange = Qmi8658AccRange_16g;
        qmi8658_config.accOdr = Qmi8658AccOdr_500Hz;
        qmi8658_config.gyrRange = Qmi8658GyrRange_2048dps;		//Qmi8658GyrRange_2048dps   Qmi8658GyrRange_1024dps
        qmi8658_config.gyrOdr = Qmi8658GyrOdr_500Hz;
        qmi8658_config.magOdr = Qmi8658MagOdr_125Hz;
        qmi8658_config.magDev = MagDev_AKM09918;
        qmi8658_config.aeOdr = Qmi8658AeOdr_128Hz;
        Qmi8658_Config_apply(&qmi8658_config);
    }
    
    return 0;
}

mt_err_t mt_esp32_gyro_init_t(void)
{
    Qmi8658_init();
    return MT_OK;
}

mt_err_t mt_esp32_gyro_update_t(void)
{
    get_acc_gyro_angle();

    /* detect acc event */
    //mt_esp32_gyro_shake_detect_t();
    mt_esp32_gyro_event_listening_t(acc_x, acc_y, acc_z);

  return MT_OK;
}


/* for gyro event machanism */
mt_err_t mt_esp32_gyro_event_init_t(void)
{
  /* 0 for shake */
  gyro_event_info[SHAKE].event_occured_flag = false;
  
  /* 1 for tilt left */
  gyro_event_info[TILT_LEFT].event_occured_flag = false;
  
  /* 2 for tilt right */
  gyro_event_info[TILT_RIGHT].event_occured_flag = false;

  /* 3 for tilt forward */
  gyro_event_info[TILT_FORWARD].event_occured_flag = false;

  /* 4 for tilt back */
  gyro_event_info[TILT_BACK].event_occured_flag = false;

  return MT_OK;
}

#define GRAVITY_DEFAULT_VALUE         (9.8)
#define TILT_RANGE_GAIN               (0.2588) // cos(75)

#define THRESHOLD_VALUE               GRAVITY_DEFAULT_VALUE*TILT_RANGE_GAIN

static mt_err_t mt_esp32_gyro_event_listening_t(float ax, float ay, float az)
{
    //ESP_LOGI(TAG, "value:%f, %f, %f", ax, ay, az);

    /* 1 for tilt left   2 for tilt right */
    if (ax <= -1 * THRESHOLD_VALUE)
    {
        gyro_event_info[TILT_LEFT].event_occured_flag = true;
        gyro_event_info[TILT_RIGHT].event_occured_flag = false;
    }
    else if (ax >= THRESHOLD_VALUE)
    {
        gyro_event_info[TILT_LEFT].event_occured_flag = false;
        gyro_event_info[TILT_RIGHT].event_occured_flag = true;
    }
    else
    {
        gyro_event_info[TILT_LEFT].event_occured_flag = false;
        gyro_event_info[TILT_RIGHT].event_occured_flag = false;
    }

    if (ay <= -1 * THRESHOLD_VALUE)
    {
        gyro_event_info[TILT_FORWARD].event_occured_flag = true;
        gyro_event_info[TILT_BACK].event_occured_flag = false;
    }
    else if (ay >= THRESHOLD_VALUE)
    {
        gyro_event_info[TILT_FORWARD].event_occured_flag = false;
        gyro_event_info[TILT_BACK].event_occured_flag = true;
    }
    else
    {
        gyro_event_info[TILT_FORWARD].event_occured_flag = false;
        gyro_event_info[TILT_BACK].event_occured_flag = false;
    }
    return MT_OK;
}

mt_err_t mt_esp32_gyro_get_tilt_status_t(uint16_t tilt_id, uint16_t *out_sta)
{

  if(gyro_event_info[tilt_id].event_occured_flag == true)
  {
    (*out_sta) = 1;
  }
  else
  {
    (*out_sta) = 0;
  }
  return MT_OK;
}

mt_err_t mt_esp32_gyro_get_acc_t(float *out_val, uint8_t axis)
{
  if(axis == AXIS_ID_X)
  {
    (*out_val) = acc_x;
  }
  else if(axis == AXIS_ID_Y)
  {
    (*out_val) = acc_y;

  }
  else if(axis == AXIS_ID_Z)
  {
    (*out_val) = acc_z;
  }
  else
  { 
    (*out_val) = 0;
    return MT_ERR_INVALID_ARG;
  }
  return MT_OK;
}

mt_err_t mt_esp32_gyro_get_gyro_t(float *out_val, uint8_t axis)
{
  if(axis == AXIS_ID_X)
  {
    (*out_val) = gyro_x;
  }
  else if(axis == AXIS_ID_Y)
  {
    (*out_val) = gyro_y;

  }
  else if(axis == AXIS_ID_Z)
  {
    (*out_val) = gyro_z;
  }
  else
  { 
    (*out_val) = 0;
    return MT_ERR_INVALID_ARG;
  }
  return MT_OK;
}
