/* pinbridge firmware adapter. [untested on hardware] */
#include "system/app_core.h"
#include "system/includes.h"
#include "app_config.h"
#include "app_action.h"
#include "usb/device/usb_stack.h"
#include "usb/usb_config.h"
#include "usb/device/cdc.h"
#include "asm/adc_api.h"
#include "pb_core.h"
/* The BR23 SDK header omits this public implementation declaration. */
extern u32 adc_get_value(u32 ch);
#if !TCFG_USB_SLAVE_CDC_ENABLE
#error "pinbridge requires USB CDC"
#endif
#define PB_USB 0
#define PB_RING 1024
static pb_core core;
static OS_SEM rx_sem;
static volatile u8 detached;
static u8 ring[PB_RING];
static volatile u16 wr,rd;
static volatile u32 drops;
static u32 last_hook;
static JL_PORT_FLASH_TypeDef *ports[4]={JL_PORTA,JL_PORTB,JL_PORTC,JL_PORTD};
static void pb_wakeup(void *dev){(void)dev;os_sem_post(&rx_sem);}
static int hal_mode(void *ctx,unsigned port,unsigned bit,unsigned mode,unsigned level){JL_PORT_FLASH_TypeDef *p=ports[port];u32 mask=1u<<bit;(void)ctx;local_irq_disable();if(mode==PB_HIZ){p->DIR|=mask;p->PU&=~mask;p->PD&=~mask;p->DIE&=~mask;p->DIEH&=~mask;}else if(mode==PB_IN||mode==PB_UP||mode==PB_DN||mode==PB_ADC){p->DIR|=mask;p->PU=(p->PU&~mask)|(mode==PB_UP?mask:0);p->PD=(p->PD&~mask)|(mode==PB_DN?mask:0);p->DIE=(p->DIE&~mask)|((mode==PB_ADC)?0:mask);p->DIEH&=~mask;}else if(mode==PB_LO||mode==PB_HI){if(level)p->OUT|=mask;else p->OUT&=~mask;p->DIE|=mask;p->DIR&=~mask;}else {local_irq_enable();return -1;}local_irq_enable();return 0;}
static void hal_write(void *ctx,unsigned port,uint16_t mask,uint16_t vals){(void)ctx;local_irq_disable();ports[port]->OUT=(ports[port]->OUT&~mask)|(vals&mask);local_irq_enable();}
static uint16_t hal_read(void *ctx,unsigned port){(void)ctx;return (uint16_t)ports[port]->IN;}
static u16 adc_registered;
static u32 adc_ready_at[16];
static int hal_adc(void *ctx,int ch,unsigned *raw,unsigned *mv){
    u32 sdk_ch, now;
    (void)ctx;
    if(ch==-2){
        if(!raw&&!mv)return 0;
        if(raw)*raw=adc_get_value(AD_CH_VBAT);
        if(mv)*mv=adc_get_voltage(AD_CH_VBAT)*4u; /* SDK VBAT channel measures one quarter of the rail */
        return 0;
    }
    if(ch<0||ch>=16)return -1;
    sdk_ch=(u32)ch;
    if(!(adc_registered&(1u<<sdk_ch))){
        adc_add_sample_ch(sdk_ch);
        adc_registered|=(u16)(1u<<sdk_ch);
        adc_ready_at[sdk_ch]=jiffies+10u; /* let the SDK's 2 ms scan loop collect a real sample */
    }
    if(!raw&&!mv)return 0; /* capability/probe call from MOD */
    now=jiffies;
    if((int32_t)(now-adc_ready_at[sdk_ch])<0)return -1;
    if(raw)*raw=adc_get_value(sdk_ch);
    if(mv)*mv=adc_get_voltage(sdk_ch);
    return 0;
}
static uint32_t hal_ms(void *ctx){(void)ctx;return jiffies*10u;}
/* Approximate busy-wait: assumes ~48 MHz and ~4 cycles per loop iteration.
 * Not a calibrated timer; do not use this as a certified heater safety timer. */
static void hal_delay_us(void *ctx,unsigned us){volatile unsigned n;(void)ctx;for(n=0;n<us*12u;n++){__asm__ volatile("nop");}}
static int hal_tx(void *ctx,const char *s,size_t n){size_t i;u16 free_space;(void)ctx;free_space=(u16)((rd+PB_RING-wr-1)%PB_RING);if(n>free_space){drops++;return -1;}for(i=0;i<n;i++){ring[wr]=(u8)s[i];wr=(u16)((wr+1)%PB_RING);}return (int)n;}
static void tx_task(void *priv){u8 b[64];(void)priv;for(;;){unsigned n=0;while(rd!=wr&&n<sizeof b){b[n++]=ring[rd];rd=(u16)((rd+1)%PB_RING);}if(n){if(!cdc_write_data(PB_USB,b,n)){} }else os_time_dly(1);}}
static void pb_step_timer(void *priv){(void)priv;pb_step_tick(&core);}
static void command_task(void *priv){u8 b[64];unsigned n,ticks=0;pb_hal h={0,hal_mode,hal_write,hal_read,hal_adc,0,hal_ms,hal_tx,hal_delay_us};(void)priv;pb_init(&core,&h);sys_s_hi_timer_add(NULL,pb_step_timer,1);for(;;){os_sem_pend(&rx_sem,1);if(detached){detached=0;pb_safe_state(&core);}for(n=0;n<32;n++){u32 got=usb_g_bulk_read(PB_USB,CDC_DATA_EP_OUT,b,sizeof b,0);if(!got)break;pb_rx(&core,b,got);}pb_poll(&core);if(++ticks>=10){ticks=0;cdc_set_wakeup_handler(pb_wakeup);}}}
static int idle_event_handler(struct application *app,struct sys_event *event){(void)app;if(event->type==SYS_DEVICE_EVENT&&event->arg==(void*)DEVICE_EVENT_FROM_OTG&&event->u.dev.event==DEVICE_EVENT_OUT)detached=1;return 0;}
static int idle_state_machine(struct application *app,enum app_state state,struct intent *it){(void)app;switch(state){case APP_STA_START:if(it&&it->action==ACTION_IDLE_MAIN){clk_set("sys",BT_NORMAL_HZ);os_sem_create(&rx_sem,0);usb_start();cdc_set_wakeup_handler(pb_wakeup);os_task_create(command_task,0,2,768,0,"pb_cmd");os_task_create(tx_task,0,1,512,0,"pb_tx");}break;case APP_STA_DESTROY:detached=1;break;default:break;}return 0;}
static const struct application_operation app_idle_ops={.state_machine=idle_state_machine,.event_handler=idle_event_handler};
REGISTER_APPLICATION(app_app_idle)={.name="idle",.action=ACTION_IDLE_MAIN,.ops=&app_idle_ops,.state=APP_STA_DESTROY};
/* Suppress all SDK logging: never leak text to CDC/UART. */
void log_print(int level,const char *tag,const char *fmt,...){(void)level;(void)tag;(void)fmt;}
int printf(const char *fmt,...){(void)fmt;return 0;}
int puts(const char *s){(void)s;return 0;}
int putchar(int c){(void)c;return 0;}
void put_buf(const u8 *buf,int len){(void)buf;(void)len;}
