#include <OneWire.h>
#include <DallasTemperature.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

/* ================== Pin mapping ================== */
const uint8_t chan_pwm  = 9;   // OC1A -> PWM 25kHz (ra base NPN qua R 4.7–10k)
const uint8_t chan_en   = 8;   // Gate MOSFET low-side (D8) de cat mass quat o Lv0
const uint8_t chan_nut  = 3;   // Nut nhan, INPUT_PULLUP (LOW = nhan)
const uint8_t chan_ds   = 4;   // DS18B20 data (kem 4.7k keo len 5V)
const uint8_t chan_tach = 2;   // Tach input (INT0), co pull-up 10k -> 5V

/* ================== LCD I2C ================== */
LiquidCrystal_I2C lcd(0x27, 16, 2); 

/* ================== DS18B20 ================== */
OneWire mot_day(chan_ds);
DallasTemperature cam_bien(&mot_day);

/* ================== Quat / tham so ================== */
// Fan 4 day chuan PC: PWM "active HIGH" tai day xanh duong.
// Voi mach NPN keo xuong, HIGH tren day PWM = NPN tat (duoc keo len 5V).
// Ta phat PWM o D9 roi DAO duty: OCR1A = (1 - duty_mong_muon) * ICR1.
const float duty_tat  = 0.00;
const float duty_muc1 = 0.30;
const float duty_muc2 = 0.60;
const float duty_muc3 = 1.00;

// Nguong Auto (co hysteresis)
const float t_tat_max  = 20.0; // <25 -> Muc 1
const float t_muc1_max = 25.0; // 25..30 -> Muc 2
const float t_muc2_max = 30.0; // >30 -> Muc 3
const float t_hyst     = 0.5;  // chong nhap nhay quanh nguong

// Thay doi muot
const float buoc_muot = 0.08;                     // thay doi toi da moi chu ky (0..1)
float duty_muc_tieu = 0.0, duty_hien_tai = 0.0;

/* ================== Trang thai =============== */
enum muc_thu_cong { M_TAT = 0, M_MUC1, M_MUC2, M_MUC3 };
muc_thu_cong muc_thu_cong_hien_tai = M_TAT;
bool che_do_tu_dong = true;

/* Nut (debounce + long-press) */
bool nut_on_dinh = HIGH;               // INPUT_PULLUP: tha = HIGH
uint32_t nut_thoi_diem_doi_ms = 0;
const uint32_t loc_nhay_ms = 30;
bool nut_da_nhan = false;
uint32_t nut_bat_dau_ms = 0;
const uint32_t giu_lau_ms = 1200;

/* Tach */
volatile uint32_t tach_xung = 0;   // dem canh FALLING
uint32_t lan_rpm_ms = 0;
uint16_t toc_do_vong = 0;

/* ======= prototypes (do dat_duty dung trong cai_dat_pwm_25khz) ======= */
void dat_duty(float d01);
void cai_dat_pwm_25khz();

/* ================== PWM 25kHz tren Timer1 (pin 9) ================== */
void cai_dat_pwm_25khz(){
  pinMode(chan_pwm, OUTPUT);                                                        
  TCCR1A = 0; TCCR1B = 0; 
  ICR1 = 639;
  TCCR1A = (1<<COM1A1) | (1<<WGM11);
  TCCR1B = (1<<WGM13)  | (1<<WGM12) | (1<<CS10);
  dat_duty(0.0);
}
// Timer1 Fast PWM mode 14, TOP = ICR1, no prescale
  // 16MHz / (1 * (1+639)) ≈ 25kHz
// DAO duty de ra "thoi gian chay" tren day PWM quat
void dat_duty(float d01){
  if (d01 < 0) d01 = 0;
  if (d01 > 1) d01 = 1;
  float dao = 1.0 - d01;                // DAO o day (active-low tai D9 -> active-high o day PWM)
  OCR1A = (uint16_t)(dao * ICR1);
}

/* ================== Auto theo nhiet (hysteresis) ================== */
muc_thu_cong muc_tu_nhiet_do(float t){
  static muc_thu_cong truoc = M_TAT;
  switch (truoc){
    case M_TAT:
      if (t >= t_tat_max + t_hyst) truoc = M_MUC1;
      break;
    case M_MUC1:
      if (t <  t_tat_max - t_hyst) truoc = M_TAT;
      else if (t >= t_muc1_max + t_hyst) truoc = M_MUC2;
      break;
    case M_MUC2:
      if (t <  t_muc1_max - t_hyst) truoc = M_MUC1;
      else if (t >= t_muc2_max + t_hyst) truoc = M_MUC3;
      break;
    case M_MUC3:
      if (t <  t_muc2_max - t_hyst) truoc = M_MUC2;
      break;
  }
  return truoc;
}

/* ================== Nut nhan ================== */
void xu_ly_nut(){
  bool muc_tho = digitalRead(chan_nut);
  uint32_t now = millis();
  if (muc_tho != nut_on_dinh && (now - nut_thoi_diem_doi_ms) > loc_nhay_ms) {
    nut_on_dinh = muc_tho; 
    nut_thoi_diem_doi_ms = now;

    if (nut_on_dinh == LOW) { // press
      nut_da_nhan = true; 
      nut_bat_dau_ms = now;
    } else if (nut_da_nhan) { // release
      uint32_t thoi_gian = now - nut_bat_dau_ms;
      if (thoi_gian >= giu_lau_ms) {
        che_do_tu_dong = !che_do_tu_dong;            // doi che do
        if (!che_do_tu_dong && muc_thu_cong_hien_tai == M_TAT) muc_thu_cong_hien_tai = M_MUC1; // vao Manual bat luon M1
      } else {
        if (!che_do_tu_dong) {
          // Manual cycle: OFF -> 1 -> 2 -> 3 -> OFF
          switch (muc_thu_cong_hien_tai){
            case M_TAT:  muc_thu_cong_hien_tai = M_MUC1; break;
            case M_MUC1: muc_thu_cong_hien_tai = M_MUC2; break;
            case M_MUC2: muc_thu_cong_hien_tai = M_MUC3; break;
            case M_MUC3: muc_thu_cong_hien_tai = M_TAT;  break;
          }
        }
      }
      nut_da_nhan = false;
    }
  }
}

/* ================== Tach ISR ================== */
void ngat_tach(){ tach_xung++; } // nhieu quat: 2 xung/vong

/* ================== Apply EN + PWM theo muc ================== */
void ap_dung_muc(uint8_t muc){
  // muc: 0..3 tuong ung Lv0..Lv3
  if (muc == 0){
    // TAT HAN: cat GND + PWM=0
    dat_duty(0.0);
    digitalWrite(chan_en, LOW);   // MOSFET OFF -> quat mat GND
  } else {
    // BAT GND truoc, roi dat PWM
    digitalWrite(chan_en, HIGH);  // MOSFET ON -> co GND
    // Kick-start neu vua bat va muc thap
    if (muc == 1){
      dat_duty(0.60); delay(300);
      dat_duty(0.30);
    } else if (muc == 2){
      dat_duty(0.60);
    } else {
      dat_duty(1.00);
    }
  }
}

/* ================== Setup ================== */
void setup() {
  pinMode(chan_nut, INPUT_PULLUP);
  pinMode(chan_tach, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(chan_tach), ngat_tach, FALLING);

  pinMode(chan_en, OUTPUT);
  digitalWrite(chan_en, LOW);  // mac dinh tat quat (cat mass)

  cai_dat_pwm_25khz();
  cam_bien.begin();

  lcd.init(); lcd.backlight();
  lcd.clear();

  Serial.begin(115200);
  lan_rpm_ms = millis();
}

/* ================== Loop ================== */
void loop() {
  xu_ly_nut();

  // Doc nhiet do (0.5s/lan)
  static uint32_t lan_doc_nhiet_ms = 0;
  static float nhiet_do_c = 25.0;
  if (millis() - lan_doc_nhiet_ms > 500) {
    cam_bien.requestTemperatures();
    float t = cam_bien.getTempCByIndex(0);
    if (t > -40 && t < 125) nhiet_do_c = t;
    lan_doc_nhiet_ms = millis();
  }

  // Xac dinh duty/muc theo Auto/Manual
  if (che_do_tu_dong) {
    muc_thu_cong muc = muc_tu_nhiet_do(nhiet_do_c);
    switch (muc){
      case M_TAT:  duty_muc_tieu = duty_tat;  break;
      case M_MUC1: duty_muc_tieu = duty_muc1; break;
      case M_MUC2: duty_muc_tieu = duty_muc2; break;
      case M_MUC3: duty_muc_tieu = duty_muc3; break;
    }
  } else {
    switch (muc_thu_cong_hien_tai){
      case M_TAT:  duty_muc_tieu = duty_tat;  break;
      case M_MUC1: duty_muc_tieu = duty_muc1; break;
      case M_MUC2: duty_muc_tieu = duty_muc2; break;
      case M_MUC3: duty_muc_tieu = duty_muc3; break;
    }
  }

  // Kick-start & Slew muot (chi thay doi duty_hien_tai; EN xu ly rieng)
  static bool dang_kick = false;
  static uint32_t kick_ms = 0;
  if (duty_hien_tai < 0.2 && duty_muc_tieu >= duty_muc1) {
    duty_hien_tai = 0.6; dat_duty(duty_hien_tai);
    dang_kick = true; kick_ms = millis();
  }
  if (dang_kick && millis() - kick_ms > 400) dang_kick = false;

  if (!dang_kick){
    if (duty_hien_tai < duty_muc_tieu) duty_hien_tai = min(duty_hien_tai + buoc_muot, duty_muc_tieu);
    else                                duty_hien_tai = max(duty_hien_tai - buoc_muot, duty_muc_tieu);
  }
    // Luu y: dat_duty(duty_hien_tai) se duoc goi trong ap_dung_muc(...) ben duoi
  // Tinh RPM moi 1s
  if (millis() - lan_rpm_ms > 1000) {
    noInterrupts(); uint32_t xung = tach_xung; tach_xung = 0; interrupts();
    toc_do_vong = (uint16_t)((xung / 2.0) * 60.0); // gia su 2 xung/vong
    lan_rpm_ms = millis();
  }

  // Xac dinh muc de dieu khien EN + PWM
  uint8_t muc_hien_thi = 0;
  if (duty_muc_tieu <= (duty_tat + 0.01)) muc_hien_thi = 0;
  else if (duty_muc_tieu < (duty_muc2 - 0.05)) muc_hien_thi = 1;
  else if (duty_muc_tieu < (duty_muc3 - 0.05)) muc_hien_thi = 2;
  else muc_hien_thi = 3;

  // Ap dung EN + PWM (goi moi vong, idempotent)
  ap_dung_muc(muc_hien_thi);
  // Dong bo hoa dat_duty voi duty_hien_tai (slew), tru khi dang Lv0
  if (muc_hien_thi > 0) dat_duty(duty_hien_tai);

  // ====== LCD ======
  lcd.setCursor(0,0);
  lcd.print(che_do_tu_dong ? "AUTO, " : "MANUAL, ");
  lcd.print("T=");
  lcd.print(nhiet_do_c,1);
  lcd.print((char)223); lcd.print("C");

  lcd.setCursor(0,1);
  lcd.print("Muc: "); lcd.print(muc_hien_thi); lcd.print(",");
  lcd.print(" PWM=");
  int duty_phan_tram = (int)(duty_muc_tieu * 100.0 + 0.5);
  if (duty_phan_tram < 100) lcd.print(" ");
  if (duty_phan_tram < 10)  lcd.print(" ");
  lcd.print(duty_phan_tram); lcd.print("% ");

  delay(100);
}