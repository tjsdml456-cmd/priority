# UE별로 목표 스루풋을 고정해 두고, 그 목표 스루풋에 수렴하도록 DSCP(→ Priority)를 PID 제어로 동적으로 조정하는 구조설명

## 목차

1. [개요](#개요)
2. [스루풋 계산](#스루풋-계산)
3. [PID 제어기](#pid-제어기)
4. [Priority 기반 제어](#priority-기반-제어)
5. [전체 제어 루프](#전체-제어-루프)
6. [코드 흐름](#코드-흐름)
7. [설정 방법](#설정-방법)

---

## 개요

### Throughput Controller란?

Throughput Controller는 각 UE의 실제 다운링크 스루풋을 측정하고, 목표 스루풋과 비교하여 PID 제어기를 통해 Priority를 조정하는 시스템입니다.

**핵심 개념:**
- **목표 스루풋 설정**: UE별로 독립적인 목표 스루풋 설정 (예: UE0=1.5Mbps, UE1=2.0Mbps)
- **실제 스루풋 측정**: 스케줄러 메트릭에서 `dl_brate_kbps` 측정
- **PID 제어기**: 오차를 계산하여 Priority 조정
- **스케줄러 반영**: Priority → PRB 할당 → TBS → 스루풋

### UE별 독립적 제어

각 UE는 독립적인 PID 상태를 가지며, 독립적으로 제어됩니다:

```
UE0: {
  목표 스루풋: 1.5Mbps
  PID 상태: {error, integral, last_error, target_priority, current_dscp}
}

UE1: {
  목표 스루풋: 2.0Mbps
  PID 상태: {error, integral, last_error, target_priority, current_dscp}
}
```

---

## 스루풋 계산

### 스루풋이란?

**`scheduler_ue_metrics.dl_brate_kbps`**: 각 UE가 실제로 받은 다운링크 비트레이트 (kbps 단위)

**코드 위치**: `lib/scheduler/logging/scheduler_metrics_handler.cpp:643, 246-254`

### 계산 방법

```cpp
// 1. HARQ ACK를 받은 TBS만 누적
void handle_dl_harq_ack(du_ue_index_t ue_index, bool ack, units::bytes tbs) {
  if (ack) {
    u.data.sum_dl_tb_bytes += tbs.value();
  }
}

// 2. 리포트 기간 동안의 평균 스루풋 계산
dl_brate_kbps = (sum_dl_tb_bytes * 8) / metric_report_period.count();
```

**공식:**
```
스루풋 (kbps) = (성공한 TBS들의 총 바이트 수 × 8) / 리포트 기간(밀리초)
```

**예시:**
- 리포트 기간: 1000ms (1초)
- 성공한 TBS 총 바이트: 150,000 bytes
- `dl_brate_kbps = (150,000 × 8) / 1000 = 1,200 kbps = 1.2 Mbps`

**중요:**
- HARQ ACK를 받은 TBS만 카운트 (NACK/타임아웃 제외)
- 리포트 기간 동안 누적된 값이므로 평균 스루풋
- 리포트 기간 종료 후 `sum_dl_tb_bytes` 리셋

### 스루풋 = TBS의 초당 양

**핵심**: 스루풋은 TBS(Transport Block Size)의 초당 양입니다.

```
스루풋 = TBS 크기 × 초당 전송 횟수
```

**TBS 결정 요소** (`lib/ran/sch/tbs_calculator.cpp:124-143`):

1. **PRB 수**: 대역폭에 의해 결정 (10MHz=52 PRB, 20MHz=106 PRB)
2. **MCS**: 변조 방식(QAM64=6 bits/symbol, QAM256=8 bits/symbol) + Code Rate
3. **레이어 수**: MIMO 구성 (1x1=1 layer, 2x2=2 layers, 4x4=4 layers)
4. **심볼 수**: 일반적으로 14 symbols per slot (PDSCH)

**TBS 계산 공식:**
```cpp
// Step 1: RE 수 계산
nof_re_prime = 12 × symbols_per_slot - DMRS_per_PRB - overhead_per_PRB
nof_re = min(156, nof_re_prime) × PRB_count

// Step 2: TBS (bits) 계산
nof_info = scaling × nof_re × code_rate × bits_per_symbol × layers
TBS_bits = quantize(nof_info)  // 표준 테이블 양자화

// Step 3: TBS (bytes) 변환
TBS_bytes = TBS_bits / 8
```

---

## PID 제어기

### PID 제어기가 필요한 이유

**문제**: 단순 비례 제어만 사용하면
- 목표에 가까워져도 약간의 오차가 남음 (정상 상태 오차)
- 오버슈트 발생 가능 (진동)

**해결**: P(비례) + I(적분) + D(미분) 조합으로 안정적이고 정확한 제어

### PID 계산식

```
PID_output = Kp × error + Ki × integral + Kd × derivative
```

### 각 항목 설명

#### 1. P (Proportional) - 비례 항

```
P_term = Kp × error
```

- **error**: 목표 스루풋 - 현재 스루풋
- **Kp**: 비례 게인 (기본값: 1.0)
- **역할**: 현재 오차에 즉각 반응

**예시:**
- error = 0.3Mbps (부족) → P_term = 1.0 × 0.3 = 0.3

#### 2. I (Integral) - 적분 항

```
I_term = Ki × integral
integral = integral + error  (누적)
```

- **integral**: 누적 오차 (과거 오차들의 합)
- **Ki**: 적분 게인 (기본값: 0.1)
- **역할**: 정상 상태 오차 제거, 지속적인 오차 보정

**예시:**
- 1초차: error = 0.3 → integral = 0.3 → I_term = 0.1 × 0.3 = 0.03
- 2초차: error = 0.2 → integral = 0.5 → I_term = 0.1 × 0.5 = 0.05
- 3초차: error = 0.1 → integral = 0.6 → I_term = 0.1 × 0.6 = 0.06

**Anti-windup**: integral을 -100 ~ +100으로 제한 (`throughput_controller.cpp:249-250`)

#### 3. D (Derivative) - 미분 항

```
D_term = Kd × (error - last_error)
```

- **derivative**: 오차 변화율 = 현재 오차 - 이전 오차
- **Kd**: 미분 게인 (기본값: 0.01)
- **역할**: 오버슈트 방지, 안정성 향상

**예시:**
- 이전 오차: 0.5Mbps
- 현재 오차: 0.3Mbps
- derivative = 0.3 - 0.5 = -0.2 (개선 중)
- D_term = 0.01 × (-0.2) = -0.002 (약간의 제동)

### 실제 계산 예시

**시나리오**: 목표 1.5Mbps, 현재 1.2Mbps

```
[1초차 계산]
- error = 1.5 - 1.2 = 0.3Mbps
- P_term = 1.0 × 0.3 = 0.3
- integral = 0.0 + 0.3 = 0.3
- I_term = 0.1 × 0.3 = 0.03
- derivative = 0.3 - 0.0 = 0.3
- D_term = 0.01 × 0.3 = 0.003
- PID_output = 0.3 + 0.03 + 0.003 = 0.333

[2초차 계산 (스루풋이 1.35Mbps로 개선)]
- error = 1.5 - 1.35 = 0.15Mbps
- P_term = 1.0 × 0.15 = 0.15
- integral = 0.3 + 0.15 = 0.45
- I_term = 0.1 × 0.45 = 0.045
- derivative = 0.15 - 0.3 = -0.15 (개선 중)
- D_term = 0.01 × (-0.15) = -0.0015
- PID_output = 0.15 + 0.045 - 0.0015 = 0.1935
```

**코드 위치**: `lib/sdap/throughput_controller.cpp:240-258`

```cpp
// Calculate error
state.error = target_mbps - current_mbps;

// Proportional term
double p_term = cfg.kp * state.error;

// Integral term (with anti-windup)
state.integral += state.error;
state.integral = std::clamp(state.integral, -max_integral, max_integral);
double i_term = cfg.ki * state.integral;

// Derivative term
double d_term = cfg.kd * (state.error - state.last_error);
state.last_error = state.error;

// PID output
double pid_output = p_term + i_term + d_term;
```

### 기본 게인 값

**기본 설정** (`include/srsran/sdap/throughput_controller.h:43-45`):
- Kp = 1.0
- Ki = 0.1
- Kd = 0.01

---

## Priority 기반 제어

### Priority의 의미

**중요**: Priority 값이 **낮을수록 높은 우선순위**입니다.

```
Priority = 5   → 최고 우선순위 (많은 PRB 할당)
Priority = 40  → 중간 우선순위
Priority = 90  → 낮은 우선순위
Priority = 127 → 최저 우선순위 (적은 PRB 할당)
```

### PID 출력을 Priority 변화로 변환

**코드 위치**: `lib/sdap/throughput_controller.cpp:283-296`

```cpp
// PID 출력을 Priority 변화로 스케일링
const double priority_scale = 10.0;  // 하드코딩된 상수 (설정 불가)
int priority_change = static_cast<int>(std::round(pid_output * priority_scale));

// 목표 Priority 계산
// 스루풋 부족 → Priority 감소 (우선순위 증가)
// 스루풋 과도 → Priority 증가 (우선순위 감소)
int target_priority = static_cast<int>(current_priority) - priority_change;
target_priority = std::clamp(target_priority, 1, 127);
```

**변환 공식:**
```
priority_change = round(PID_output × 10.0)
target_priority = current_priority - priority_change
target_priority = clamp(target_priority, 1, 127)
```

**중요:**
- `priority_scale = 10.0`은 **하드코딩된 상수**입니다 (설정 불가)
- 이것은 PID 출력을 Priority 변화량으로 변환하는 **스케일 팩터**입니다
- PID 파라미터(Kp, Ki, Kd)는 여전히 설정 가능합니다
- `priority_scale`은 PID 출력 1.0이 Priority 변화 10에 해당하도록 변환합니다

**예시:**
- 현재 Priority = 40
- PID_output = 0.3 (스루풋 부족)
- priority_change = round(0.3 × 10.0) = 3
- target_priority = 40 - 3 = 37 (우선순위 증가)

**의미:**
- PID_output이 1.0이면 Priority가 10만큼 변화
- PID_output이 0.5이면 Priority가 5만큼 변화
- PID_output이 2.0이면 Priority가 20만큼 변화

### 스케줄러에서 Priority 적용

**코드 위치**: `lib/scheduler/policy/scheduler_time_qos.cpp:379-409`

```cpp
auto& throughput_ctrl = throughput_controller::get_instance();
std::optional<uint8_t> target_priority = throughput_ctrl.get_target_priority(ue_index);

if (target_priority.has_value()) {
  // Throughput Controller가 활성화되어 있으면 계산된 Priority 직접 사용
  effective_priority = qos_prio_level_t{target_priority.value()};
} else {
  // 비활성화되어 있으면 DSCP → 5QI → Priority 경로 사용
  // ...
}
```

**중요:**
- Throughput Controller가 계산한 `target_priority`를 스케줄러에서 직접 사용
- DSCP → 5QI → Priority 매핑 경로를 우회
- Priority 범위 1-127 전체 사용 가능

### Priority → PRB 할당

스케줄러는 Priority를 사용하여 `final_priority`를 계산하고, 이를 기준으로 UE 선택 및 PRB 할당:

```
Priority (낮을수록 높은 우선순위)
  ↓
combined_prio = priority × ARP_priority
  ↓
prio_weight 계산
  ↓
final_priority = pf_weight × gbr_weight × prio_weight × delay_weight
  ↓
스케줄러가 final_priority 높은 UE를 우선 선택
  ↓
더 많은 PRB 할당
  ↓
더 큰 TBS
  ↓
더 높은 스루풋
```

---

## 전체 제어 루프

### 제어 루프 다이어그램

```
[초기화] UE별 목표 스루풋 설정
  파일: apps/du/du.cpp 또는 apps/gnb/gnb.cpp
  ↓
  throughput_ctrl.set_target_throughput_map({
    {UE0, 1.5Mbps},
    {UE1, 2.0Mbps},
    {UE2, 1.0Mbps}
  })
  ↓

[1단계] 스루풋 측정 (주기적으로, 예: 1초마다)
  파일: lib/scheduler/logging/scheduler_metrics_handler.cpp
  ↓
  dl_brate_kbps = (sum_dl_tb_bytes × 8) / metric_report_period.count()
  ↓

[2단계] Throughput Controller에 전달
  파일: lib/scheduler/logging/scheduler_metrics_handler.cpp
  ↓
  throughput_ctrl.update_throughput(metrics)
  ↓

[3단계] 오차 계산
  파일: lib/sdap/throughput_controller.cpp
  ↓
  error = target_mbps - current_mbps
  current_mbps = metrics.dl_brate_kbps / 1000.0
  ↓

[4단계] PID 제어기 계산
  파일: lib/sdap/throughput_controller.cpp
  ↓
  PID_output = Kp×error + Ki×integral + Kd×derivative
  ↓

[5단계] Priority 계산
  파일: lib/sdap/throughput_controller.cpp
  ↓
  priority_change = round(PID_output × 10.0)
  target_priority = current_priority - priority_change
  target_priority = clamp(target_priority, 1, 127)
  ↓

[6단계] target_priority 저장
  파일: lib/sdap/throughput_controller.cpp
  ↓
  state.target_priority = target_priority
  ↓

[7단계] 스케줄러에서 Priority 적용
  파일: lib/scheduler/policy/scheduler_time_qos.cpp
  ↓
  target_priority = throughput_ctrl.get_target_priority(ue_index)
  effective_priority = target_priority.value()
  ↓

[8단계] 스케줄러가 PRB 할당
  파일: lib/scheduler/policy/scheduler_time_qos.cpp
  ↓
  final_priority 계산
  우선순위 높은 UE 선택
  PRB 할당
  ↓

[9단계] TBS 결정 및 전송
  파일: lib/ran/sch/tbs_calculator.cpp
  ↓
  TBS = f(PRB, MCS, 레이어, ...)
  전송
  ↓

[다음 주기] 다시 [1단계]로 (1초 후)
```

## 코드 흐름

### 1. 초기화

**파일**: `apps/du/du.cpp:412-419` 또는 `apps/gnb/gnb.cpp:550-557`

```cpp
auto& throughput_ctrl = throughput_controller::get_instance();
std::unordered_map<du_ue_index_t, double> ue_target_throughput_map = {
    {to_du_ue_index(0), 1.5},  // UE0: 1.5Mbps
    {to_du_ue_index(1), 2.0},  // UE1: 2.0Mbps
    {to_du_ue_index(2), 1.0}   // UE2: 1.0Mbps
};
throughput_ctrl.set_target_throughput_map(ue_target_throughput_map);
```

**중요**: UE 등록 **전에** 설정해야 UE가 등록될 때 자동으로 적용됨

### 2. 스루풋 측정 및 전달

**파일**: `lib/scheduler/logging/scheduler_metrics_handler.cpp:643, 500`

```cpp
// 스루풋 계산
ret.dl_brate_kbps = static_cast<double>(data.sum_dl_tb_bytes * 8U) / metric_report_period.count();

// Throughput Controller에 전달
throughput_ctrl.update_throughput(metrics);
```

### 3. PID 제어기 계산

**파일**: `lib/sdap/throughput_controller.cpp:240-296`

```cpp
void throughput_controller::compute_dscp_adjustment(pid_state& state, double current_mbps, double target_mbps) {
  // 1. 오차 계산
  state.error = target_mbps - current_mbps;
  
  // 2. PID 계산
  double p_term = cfg.kp * state.error;
  state.integral += state.error;
  state.integral = std::clamp(state.integral, -max_integral, max_integral);
  double i_term = cfg.ki * state.integral;
  double d_term = cfg.kd * (state.error - state.last_error);
  state.last_error = state.error;
  double pid_output = p_term + i_term + d_term;
  
  // 3. Priority 계산
  const double priority_scale = 10.0;
  int priority_change = static_cast<int>(std::round(pid_output * priority_scale));
  int target_priority = static_cast<int>(current_priority) - priority_change;
  target_priority = std::clamp(target_priority, 1, 127);
  state.target_priority = static_cast<uint8_t>(target_priority);
}
```

### 4. 스케줄러에서 Priority 적용

**파일**: `lib/scheduler/policy/scheduler_time_qos.cpp:379-409`

```cpp
auto& throughput_ctrl = throughput_controller::get_instance();
std::optional<uint8_t> target_priority = throughput_ctrl.get_target_priority(ue_index);

qos_prio_level_t effective_priority;

if (target_priority.has_value()) {
  // Throughput Controller가 활성화되어 있으면 계산된 Priority 직접 사용
  effective_priority = qos_prio_level_t{target_priority.value()};
} else {
  // 비활성화되어 있으면 DSCP → 5QI → Priority 경로 사용
  // ...
}
```

---

## 설정 방법

### 기본 설정

**파일**: `include/srsran/sdap/throughput_controller.h:40-48`

```cpp
struct config {
  double target_throughput_mbps = 1.5;  // 기본 목표 스루풋
  double kp = 1.0;   // Proportional gain
  double ki = 0.1;   // Integral gain
  double kd = 0.01;  // Derivative gain
  std::chrono::milliseconds control_period_ms{1000};  // 제어 주기 (1초)
  uint8_t min_dscp = 0;
  uint8_t max_dscp = 63;
  uint8_t initial_dscp = 32;
  bool enabled = true;
};
```

### UE별 목표 스루풋 설정

**파일**: `apps/du/du.cpp:412-419` 또는 `apps/gnb/gnb.cpp:550-557`

```cpp
auto& throughput_ctrl = throughput_controller::get_instance();
std::unordered_map<du_ue_index_t, double> ue_target_map = {
  {to_du_ue_index(0), 1.5},  // UE0: 1.5Mbps
  {to_du_ue_index(1), 2.0},  // UE1: 2.0Mbps
  {to_du_ue_index(2), 1.0}   // UE2: 1.0Mbps
};
throughput_ctrl.set_target_throughput_map(ue_target_map);
```


