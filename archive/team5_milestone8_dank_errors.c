/*
 *************************************************************************************************************
 * BIOS HEADER FILES
 *************************************************************************************************************
 */
#include <xdc/std.h>                        //mandatory - have to include first, for BIOS types
#include <ti/sysbios/BIOS.h>                //mandatory - if you call APIs like BIOS_start()
#include <xdc/runtime/Log.h>                //needed for any Log_info() call
#include <xdc/cfg/global.h>                 //header file for statically defined objects/handles
#include <xdc/runtime/Timestamp.h>          // used for Timestamp() calls
/*
 *************************************************************************************************************
 * C HEADER FILES
 *************************************************************************************************************
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
/*
 *************************************************************************************************************
 * TIVA C HEADER FILES
 *************************************************************************************************************
 */
#include "inc/hw_memmap.h"
#include "inc/hw_types.h"
#include "driverlib/debug.h"
#include "driverlib/fpu.h"
#include "driverlib/gpio.h"
#include "driverlib/pin_map.h"
#include "driverlib/rom.h"
#include "driverlib/sysctl.h"
#include "driverlib/adc.c"
#include "driverlib/uart.c"
#include "utils/uartstdio.h"
#include "driverlib/interrupt.c"
#include "driverlib/pwm.c"
#include "inc/hw_ints.h"
#include "driverlib/timer.c"
#include "driverlib/udma.h"
#include "inc/hw_udma.h"
#include "inc/hw_uart.h"
#include "driverlib/systick.h"

/*
 *************************************************************************************************************
 * DEFINING CONSTANTS
 *************************************************************************************************************
 */
#define PWM_FREQUENCY       10000
#define PWM_ADJUST          83
#define ADC_MAX_VALUE       4096
#define TARGET_VALUE        (ADC_MAX_VALUE / 2) //2000
#define SEQUENCE_ONE        1
#define SEQUENCE_TWO        2
#define SEQUENCE_THREE      3
#define SEQUENCE_FOUR       4
#define PRIORITY_ZERO       0
#define PRIORITY_ONE        1
#define STEP_ZERO           0
#define BUFFER_SIZE         20

/*
 *************************************************************************************************************
 * GLOBAL VARIABLES
 *************************************************************************************************************
 */
char command[2] = "  "; //2-char command array
uint32_t ForwardSensor[1];

/*
 *************************************************************************************************************
 * ADC VALUE
 *************************************************************************************************************
 */
uint32_t rightSensorValue = 0;
uint32_t frontSensorValue = 0;

/*
 *************************************************************************************************************
 * PWM VALUES
 *************************************************************************************************************
 */
volatile uint32_t PWM_CLOCK, PWM_LOAD;
volatile uint32_t TWO_SECONDS = 80000000;

/*
 *************************************************************************************************************
 * PID VALUES
 *************************************************************************************************************
 */
volatile float proportionalRight;
volatile float lastProportionalRight = 250;
volatile float integralRight = 0;
volatile float derivativeRight;
volatile float pidRight;

/*
 *************************************************************************************************************
 * DECLARING FUNCTIONS
 *************************************************************************************************************
 */
void ConfigurePeripherals(void);
void ConfigureUART(void);
void ConfigureADC(void);
void ConfigurePWM(void);
void distanceSensorCalculations(uint32_t rValue, uint32_t fValue);
void distanceSensorTest(void);
void lightSensorTest(void);
void motorTest(void);
void PID(int RightValue, int FrontValue);
void Timer2IntHandler(void);
void delayMS(int ms);

/*
 *************************************************************************************************************
 * INITIALIZE EVERYTHING
 *************************************************************************************************************
 */
void ConfigurePeripherals(void) {

    FPULazyStackingEnable();

    /* Set system clock */
    SysCtlClockSet(
            SYSCTL_SYSDIV_5 | SYSCTL_USE_PLL | SYSCTL_XTAL_16MHZ | SYSCTL_OSC_MAIN);

    ConfigureUART();
    ConfigurePWM();
    ConfigureADC();
}

/*
 *************************************************************************************************************
 * UART CONFIG
 *************************************************************************************************************
 */
void ConfigureUART(void)
{
    /* Enable the clocks to PortB and UART1 */
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOB);
    SysCtlPeripheralEnable(SYSCTL_PERIPH_UART1);

    /* Configure PortB pins 0 & 1 for UART1 RX & TX, respectively */
    GPIOPinConfigure(GPIO_PB0_U1RX); //goes to TX pin
    GPIOPinConfigure(GPIO_PB1_U1TX); //goes to RX pin
    GPIOPinTypeUART(GPIO_PORTB_BASE, GPIO_PIN_0 | GPIO_PIN_1);

    /* Set the UART1 module's clock source */
    UARTClockSourceSet(UART1_BASE, UART_CLOCK_PIOSC);
    UARTConfigSetExpClk(UART1_BASE, SysCtlClockGet(), 115200,
                        (UART_CONFIG_WLEN_8 | UART_CONFIG_STOP_ONE |
                                UART_CONFIG_PAR_NONE));

    /* Configure UART1 */
    // UART module: 1
    // Baud rate: 115200
    // UART clock speed: 16 [MHz]
    UARTStdioConfig(1, 115200, 16000000);
    UARTIntEnable(UART1_BASE, UART_INT_RX | UART_INT_TX);
}

/*
 *************************************************************************************************************
 * ADC CONFIG
 *************************************************************************************************************
 */
void ConfigureADC(void) {

    /* Enable the clock for ADC0 and PortE */
    SysCtlPeripheralEnable(SYSCTL_PERIPH_ADC0);
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOE);

    /* Configure PortE pins 2 & 3 for ADC usage */
    GPIOPinTypeADC(GPIO_PORTE_BASE, GPIO_PIN_3 | GPIO_PIN_2); //pin3 = right sensor, pin2 = front sensor

    /* Disable sequencers to ensure safe reconfiguration of them */
    ADCSequenceDisable(ADC0_BASE, 1); //disable sequence 1
    ADCSequenceDisable(ADC0_BASE, 2); //disable sequence 2
    ADCSequenceDisable(ADC0_BASE, 3); //disable sequence 3
    ADCSequenceDisable(ADC0_BASE, 4); //disable sequence 4

    /* Configure sequence priorities and triggers */
    // SS0: unused
    // SS1 - to sample right sensor: trigger = timer, priority = 0
    // SS2 - to sample front sensor: trigger = timer, priority = 1
    // SS3: unused
    ADCSequenceConfigure(ADC0_BASE, SEQUENCE_ONE, ADC_TRIGGER_PROCESSOR,
                         PRIORITY_ZERO);
    ADCSequenceConfigure(ADC0_BASE, SEQUENCE_TWO, ADC_TRIGGER_PROCESSOR,
                         PRIORITY_ONE);

    /* Configuring sequence steps for sequence 1 */
    // Step 0: sample right sensor, end of sequence
    ADCSequenceStepConfigure(ADC0_BASE, SEQUENCE_ONE, STEP_ZERO,
                             (ADC_CTL_CH0 | ADC_CTL_END));

    /* Configuring sequence steps for sequence 2 */
    // Step 0: sample front sensor, end of sequence
    ADCSequenceStepConfigure(ADC0_BASE, SEQUENCE_TWO, STEP_ZERO,
                             (ADC_CTL_CH1 | ADC_CTL_END));

    /* Re-enable our now newly configured sequences */
    ADCSequenceEnable(ADC0_BASE, SEQUENCE_ONE);
    ADCSequenceEnable(ADC0_BASE, SEQUENCE_TWO);
}

/*
 *************************************************************************************************************
 * PWM CONFIG
 *************************************************************************************************************
 */
void ConfigurePWM(void)
{
    /* Set the PWM module's clock divider */
    SysCtlPWMClockSet(SYSCTL_PWMDIV_64);

    /* Enable the clock for PWM1 and PortA, PortF, PortB */
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOA);
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOF);
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOB);
    SysCtlPeripheralEnable(SYSCTL_PERIPH_PWM1);

    //Phase pins and mode
    GPIOPinTypeGPIOOutput(GPIO_PORTF_BASE, GPIO_PIN_1);
    GPIOPinTypeGPIOOutput(GPIO_PORTB_BASE, GPIO_PIN_6|GPIO_PIN_7);
    GPIOPinWrite(GPIO_PORTF_BASE, GPIO_PIN_1, GPIO_PIN_1); //L motor set forward
    GPIOPinWrite(GPIO_PORTB_BASE, GPIO_PIN_6, GPIO_PIN_6); //R motor set forward
    GPIOPinWrite(GPIO_PORTB_BASE, GPIO_PIN_7, GPIO_PIN_7); //PHASE/ENABLE Mode

    /* Configure PortA pins 6 & 7 for PWM module 1, generator 1 usage */
    GPIOPinConfigure(GPIO_PA6_M1PWM2);
    GPIOPinConfigure(GPIO_PA7_M1PWM3);
    GPIOPinTypePWM(GPIO_PORTA_BASE, GPIO_PIN_6 | GPIO_PIN_7);

    /* Configure M1PWM0 for count down mode */
    PWMGenConfigure(PWM1_BASE, PWM_GEN_1, PWM_GEN_MODE_DOWN);

    /* Calculate the PWM clock and load values */
    PWM_CLOCK = SysCtlClockGet() / 64;
    PWM_LOAD = (PWM_CLOCK / PWM_FREQUENCY) - 1;

    /* Set the period of the PWM generator */
    PWMGenPeriodSet(PWM1_BASE, PWM_GEN_1, PWM_LOAD);
    //PWMGenPeriodSet(PWM1_BASE, PWM_GEN_1, 10000); //for manual motor control

    /* Specify the duty cycle for the PWM signal */
    PWMPulseWidthSet(PWM1_BASE, PWM_OUT_2, PWM_ADJUST * PWM_LOAD / 100); //left motor
    PWMPulseWidthSet(PWM1_BASE, PWM_OUT_3, PWM_ADJUST * PWM_LOAD / 100); //right motor

    /* Enable PWM output */
    PWMOutputState(PWM1_BASE, PWM_OUT_2_BIT | PWM_OUT_3_BIT, true);

    /* Enable the timer/counter for M1PWM0 */
    PWMGenEnable(PWM1_BASE, PWM_GEN_1);

}

/*
 *************************************************************************************************************
 * SET DELAY IN MS
 *************************************************************************************************************
 */
void delayMS(int ms) {
    SysCtlDelay( (SysCtlClockGet()/(3*1000))*ms ) ;
}

/*
 *************************************************************************************************************
 * MEASURE IN CM
 *************************************************************************************************************
 */
void distanceSensorCalculations(uint32_t rValue, uint32_t fValue) {
    float rVolts, fVolts = 0.0;
    uint32_t rDistance, fDistance = 0;
    rVolts = (rValue * 0.000732421875);
    fVolts = (fValue * 0.000732421875);
    rDistance = (13*pow(rVolts, -1)) - 1;
    fDistance = (13*pow(fVolts, -1)) - 1;
    //UARTprintf("Right Sensor: not measuring, Front Sensor: %i cm\n", rDistance, fDistance);
    SysCtlDelay(3000);
    UARTprintf("Right Sensor: %i cm, Front Sensor: %i cm\n", rDistance, fDistance);
}

/*
 *************************************************************************************************************
 * LIGHT SENSOR TEST
 *************************************************************************************************************
 */
void lightSensorTest(void) {
    uint32_t lightSensorValue = 0;
    uint32_t counter = 0;
    while(1) {
        GPIOPinTypeGPIOOutput(GPIO_PORTF_BASE, GPIO_PIN_4);
        GPIOPinWrite(GPIO_PORTF_BASE, GPIO_PIN_4, 0x00000010);
        SysCtlDelay(100);
        GPIOPinTypeGPIOInput(GPIO_PORTF_BASE, GPIO_PIN_4);
        while (GPIOPinRead(GPIO_PORTF_BASE, GPIO_PIN_4) != 0) {
            counter++;
        }
        lightSensorValue = counter;
        UARTprintf("%d\n", lightSensorValue);
        counter = 0;
        SysCtlDelay(5000);
    }
}

/*
 *************************************************************************************************************
 * MANUAL MOTOR TEST (set PWM frequency to 10000)
 *************************************************************************************************************
 */
void motorTest(void) { //only works if you set period to 10000
    unsigned long pwmNow = 5000;

    while(true) {
        UARTprintf("\n");
        UARTprintf("The folliwng is a list of commands:\n"
                "01 - Stop\n"
                "02 - Reverse\n"
                "03 - Forward\n"
                "04 - Speed Up\n"
                "05 - Slow Down\n"
                "06 - Turn Right\n"
                "07 - Turn Left\n");
        UARTgets(command, strlen(command) + 1);
        UARTprintf("\n");
        if (!strcmp(command, "1"))    // Stop
        {
            pwmNow = 5000;
            PWMPulseWidthSet(PWM1_BASE, PWM_OUT_2, 10);
            PWMPulseWidthSet(PWM1_BASE, PWM_OUT_3, 10);
            UARTprintf("speed is %d \n", pwmNow);
        }
        if (!strcmp(command, "2"))    // Reverse
        {
            GPIOPinWrite(GPIO_PORTF_BASE, GPIO_PIN_1, 0);
            GPIOPinWrite(GPIO_PORTB_BASE, GPIO_PIN_6, 0);
            PWMPulseWidthSet(PWM1_BASE, PWM_OUT_2,pwmNow);
            PWMPulseWidthSet(PWM1_BASE, PWM_OUT_3,pwmNow);
            UARTprintf("speed is %d \n", pwmNow);
        }
        if (!strcmp(command, "3"))    // Forward
        {
            GPIOPinWrite(GPIO_PORTF_BASE, GPIO_PIN_1, GPIO_PIN_1);
            GPIOPinWrite(GPIO_PORTB_BASE, GPIO_PIN_6, GPIO_PIN_6);
            PWMPulseWidthSet(PWM1_BASE, PWM_OUT_2,pwmNow);
            PWMPulseWidthSet(PWM1_BASE, PWM_OUT_3,pwmNow);
            UARTprintf("speed is %d \n", pwmNow);
        }
        if (!strcmp(command, "4"))    // Speed Up
        {
            pwmNow += 2500;
            PWMPulseWidthSet(PWM1_BASE, PWM_OUT_2,pwmNow);
            PWMPulseWidthSet(PWM1_BASE, PWM_OUT_3,pwmNow);
            UARTprintf("speed is %d \n", pwmNow);
        }
        if (!strcmp(command, "5"))    // Slow down
        {
            pwmNow -= 2500;
            PWMPulseWidthSet(PWM1_BASE, PWM_OUT_2,pwmNow);
            PWMPulseWidthSet(PWM1_BASE, PWM_OUT_3,pwmNow);
            UARTprintf("speed is %d \n", pwmNow);
        }
        if (!strcmp(command, "6"))    // Turn Right
        {
            PWMPulseWidthSet(PWM1_BASE, PWM_OUT_2,pwmNow); //left
            PWMPulseWidthSet(PWM1_BASE, PWM_OUT_3,pwmNow - 2500); //right
            UARTprintf("speed is %d \n", pwmNow);
        }
        if (!strcmp(command, "7"))    // Turn Left
        {
            PWMPulseWidthSet(PWM1_BASE, PWM_OUT_2,pwmNow - 2500);
            PWMPulseWidthSet(PWM1_BASE, PWM_OUT_3,pwmNow);
            UARTprintf("speed is %d \n", pwmNow);
        }

    }
}

/*
 *************************************************************************************************************
 * DISPLAY DISTANCE & PID VALUES W/O MOTORS
 *************************************************************************************************************
 */
void distanceSensorTest(void) {
    while (1) {
        /* Trigger the sample sequence 1 */
        ADCProcessorTrigger(ADC0_BASE, SEQUENCE_ONE);

        /* Get results from sample sequence 1 */
        ADCSequenceDataGet(ADC0_BASE, SEQUENCE_ONE, &rightSensorValue); //add to watch expression

        /* Trigger sample sequence 2 */
        ADCProcessorTrigger(ADC0_BASE, SEQUENCE_TWO);

        /* Get results from sample sequence 2 */
        ADCSequenceDataGet(ADC0_BASE, SEQUENCE_TWO, &frontSensorValue); //add to watch expression

        int RightValue = rightSensorValue; //add to watch expression
        int FrontValue = frontSensorValue; //add to watch expression

        /* Calculate proportional terms */
        proportionalRight = (RightValue - TARGET_VALUE) / 20;

        /* Calculate integral terms */
        integralRight = (RightValue - TARGET_VALUE);

        /* Calculate derivative terms */
        derivativeRight = ((RightValue - TARGET_VALUE) - lastProportionalRight)
                                                                            * (3 / 2);

        /* Calculate PID result */
        pidRight = proportionalRight + (integralRight / 10000) + derivativeRight;

        /* Update some values for proper calculations of the next PID update*/
        lastProportionalRight = (RightValue - TARGET_VALUE);

        if ((pidRight < 25) && (FrontValue > 2000)) // check if dead end & U-Turn
        {
            UARTprintf("U-Turn\n");
        }
        else if (pidRight > 27 && FrontValue < 1000)  // Turn Left
        {
            UARTprintf("Slight Left\n");
        }
        else if (pidRight > -80 && pidRight < -20 && FrontValue < 1000) // Turn Right
        {
            UARTprintf("Slight Right\n");
        }
        else if (pidRight < -100 && FrontValue < 1200) // SHARP RIGHT
        {
            UARTprintf("************************RIGHT************************\n");
        }
        else if ((pidRight > -20 && pidRight < 20) && FrontValue < 1800)// Go straight
        {
            UARTprintf("Straight\n");
        }
        else if ((pidRight > -40 && pidRight < 0) && (FrontValue > 1000) && (FrontValue < 1400))// Special straight
                {
                    UARTprintf("=================Straight================\n");
                }
        else
        {
            UARTprintf("No change\n");
        }

        delayMS(50); //wait 50ms
    }
}

/*
 *************************************************************************************************************
 * TIMER2 INTERRUPT FUNCTION
 *************************************************************************************************************
 */
void Timer2IntHandler(void) {

    /* Trigger the sample sequence 1 */
    ADCProcessorTrigger(ADC0_BASE, SEQUENCE_ONE);

    /* Get results from sample sequence 1 */
    ADCSequenceDataGet(ADC0_BASE, SEQUENCE_ONE, &rightSensorValue);

    /* Trigger sample sequence 2 */
    ADCProcessorTrigger(ADC0_BASE, SEQUENCE_TWO);

    /* Get results from sample sequence 2 */
    ADCSequenceDataGet(ADC0_BASE, SEQUENCE_TWO, &frontSensorValue);

    /* Perform PID on the sensor values */
    PID(rightSensorValue, frontSensorValue);
}

/*
 *************************************************************************************************************
 * PID (set PWM frequency to PWM_LOAD)
 *************************************************************************************************************
 */
void PID(int RightValue, int FrontValue) {

    //UARTprintf ("The error value: %d\n\r", error);
    proportionalRight = (RightValue - TARGET_VALUE) / 20;

    /* Calculate integral terms */
    integralRight = (RightValue - TARGET_VALUE);

    /* Calculate derivative terms */
    derivativeRight = ((RightValue - TARGET_VALUE) - lastProportionalRight)
                                                * (3 / 2);

    /* Calculate PID result */
    pidRight = proportionalRight + (integralRight / 10000) + derivativeRight;

    /* Update some values for proper calculations of the next PID update*/
    lastProportionalRight = (RightValue - TARGET_VALUE);

    if ((pidRight < 25) && (FrontValue > 2000)) // check if dead end & U-Turn
    {
        GPIOPinWrite(GPIO_PORTF_BASE, GPIO_PIN_1, 0); //left backward
        GPIOPinWrite(GPIO_PORTB_BASE, GPIO_PIN_6, GPIO_PIN_6); //right forward

        PWMPulseWidthSet(PWM1_BASE, PWM_OUT_2, 99 * PWM_LOAD / 100);
        PWMPulseWidthSet(PWM1_BASE, PWM_OUT_3, 99 * PWM_LOAD / 100);
        //UARTprintf("U-Turn\n");

    }
    else if (pidRight > 27 && FrontValue < 1000)  // Turn Left
    {
        GPIOPinWrite(GPIO_PORTF_BASE, GPIO_PIN_1, GPIO_PIN_1); //forward
        GPIOPinWrite(GPIO_PORTB_BASE, GPIO_PIN_6, GPIO_PIN_6); //forward

        PWMPulseWidthSet(PWM1_BASE, PWM_OUT_2, 70 * PWM_LOAD / 100); //left slow
        PWMPulseWidthSet(PWM1_BASE, PWM_OUT_3, 80 * PWM_LOAD / 100); //right
        //UARTprintf("Slight Left\n");
    }
    else if (pidRight > -80 && pidRight < -20 && FrontValue < 1000) // Turn Right //-25
    {
        GPIOPinWrite(GPIO_PORTF_BASE, GPIO_PIN_1, GPIO_PIN_1); //forward
        GPIOPinWrite(GPIO_PORTB_BASE, GPIO_PIN_6, GPIO_PIN_6); //forward

        PWMPulseWidthSet(PWM1_BASE, PWM_OUT_2, 80 * PWM_LOAD / 100); //left
        PWMPulseWidthSet(PWM1_BASE, PWM_OUT_3, 70 * PWM_LOAD / 100); //right slow
        //UARTprintf("Slight Right\n");
    }
    else if (pidRight < -100 && FrontValue < 1400) // SHARP RIGHT //-70
    {
        GPIOPinWrite(GPIO_PORTF_BASE, GPIO_PIN_1, GPIO_PIN_1); //forward
        GPIOPinWrite(GPIO_PORTB_BASE, GPIO_PIN_6, GPIO_PIN_6); //forward

        PWMPulseWidthSet(PWM1_BASE, PWM_OUT_2, 99 * PWM_LOAD / 100); //left
        PWMPulseWidthSet(PWM1_BASE, PWM_OUT_3, 17 * PWM_LOAD / 100); //right slow
        //UARTprintf("************************RIGHT************************\n");
        SysCtlDelay(500);
    }
    else if ((pidRight > -20 && pidRight < 20) && FrontValue < 1800)// Go straight
    {
        GPIOPinWrite(GPIO_PORTF_BASE, GPIO_PIN_1, GPIO_PIN_1); //forward
        GPIOPinWrite(GPIO_PORTB_BASE, GPIO_PIN_6, GPIO_PIN_6); //forward

        PWMPulseWidthSet(PWM1_BASE, PWM_OUT_2, 80 * PWM_LOAD / 100);
        PWMPulseWidthSet(PWM1_BASE, PWM_OUT_3, 80 * PWM_LOAD / 100);
        //UARTprintf("Straight\n");
    }
    else if ((pidRight > -50 && pidRight < 0) && (FrontValue > 1000) && (FrontValue < 1500))// Special straight
    {
        GPIOPinWrite(GPIO_PORTF_BASE, GPIO_PIN_1, GPIO_PIN_1); //forward
        GPIOPinWrite(GPIO_PORTB_BASE, GPIO_PIN_6, GPIO_PIN_6); //forward

        PWMPulseWidthSet(PWM1_BASE, PWM_OUT_2, 80 * PWM_LOAD / 100);
        PWMPulseWidthSet(PWM1_BASE, PWM_OUT_3, 80 * PWM_LOAD / 100);
        //UARTprintf("=================Straight================\n");
    }
    //light sensor portion
    uint32_t lightSensorValue = 0;
    uint32_t counter = 0;
    GPIOPinTypeGPIOOutput(GPIO_PORTF_BASE, GPIO_PIN_4);
    GPIOPinWrite(GPIO_PORTF_BASE, GPIO_PIN_4, 0x00000010);
    SysCtlDelay(100);
    GPIOPinTypeGPIOInput(GPIO_PORTF_BASE, GPIO_PIN_4);
    while (GPIOPinRead(GPIO_PORTF_BASE, GPIO_PIN_4) != 0) {
        counter++;
    }
    lightSensorValue = counter;
    UARTprintf("%u", lightSensorValue);
    if (lightSensorValue < 2000) {
        UARTprintf(" white\n");
    }
    else {
        UARTprintf(" black\n");
        //PWMOutputState(PWM1_BASE, PWM_OUT_2_BIT | PWM_OUT_3_BIT, false); //stop motors
    }
    counter = 0;
}

/*
 *************************************************************************************************************
 * MAIN
 *************************************************************************************************************
 */
 int main(void) {

    /* Initialize everything */
    ConfigurePeripherals();

    /* Turn off motor to prevent robot going rogue */
    PWMOutputState(PWM1_BASE, PWM_OUT_2_BIT | PWM_OUT_3_BIT, false);

    /* Menu */
    while(true) {

        UARTprintf("\n");
        UARTprintf("Version 7.3\n");
        UARTprintf("The folliwng is a list of commands:\n"
                "DS - run distance sensor test\n"
                "LS - run light sensor test\n"
                "MS - test motors\n"
                "PD - PID test\n");
        UARTgets(command, strlen(command) + 1);
        UARTprintf("\n");

        if (!strcmp(command, "DS"))    // Start
        {
            distanceSensorTest();
        }
        if (!strcmp(command, "LS"))    // Start
        {
            lightSensorTest();
        }
        if (!strcmp(command, "MS"))    // Start
        {
            UARTprintf("Starting motors...\n\r");
            PWMOutputState(PWM1_BASE, PWM_OUT_2_BIT | PWM_OUT_3_BIT, true);
            motorTest();
        }
        if (!strcmp(command, "PD"))    // Start
        {
            PWMOutputState(PWM1_BASE, PWM_OUT_2_BIT | PWM_OUT_3_BIT, true);
            /* Initialize timer */
            BIOS_start();
        }
    }


    /*
    while (true) {
         //do nothing
    }*/
}
