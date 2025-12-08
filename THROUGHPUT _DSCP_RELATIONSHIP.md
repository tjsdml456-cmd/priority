# Throughput과 DSCP 조정 관계 설명

## 핵심 개념

### 1. UE별 독립적인 제어

**각 UE는 독립적으로 제어됩니다:**

```
UE0: {
  목표 스루풋: 5Mbps
  현재 스루풋: 4.2Mbps (스케줄러 메트릭에서 측정)
  DSCP: 32 → 38 (PID 제어기로 조정)
  PID 상태: {error, integral, last_error}
}

UE1: {
  목표 스루풋: 5Mbps  
  현재 스루풋: 5.8Mbps
  DSCP: 32 → 28 (PID 제어기로 조정)
  PID 상태: {error, integral, last_error}
}

UE2: {
  목표 스루풋: 5Mbps
  현재 스루풋: 5.0Mbps
  DSCP: 32 (변화 없음)
  PID 상태: {error, integral, last_error}
}
```

### 2. Throughput이란?

**`scheduler_ue_metrics.dl_brate_kbps`**: 
- 각 UE가 **실제로 받은** 다운링크 비트레이트 (kbps 단위)
- 스케줄러가 할당한 리소스로 전송된 데이터의 실제 스루풋
- 주기적으로 측정됨 (예: 1초마다)

**예시:**
- UE0: `dl_brate_kbps = 4200` → 4.2Mbps
- UE1: `dl_brate_kbps = 5800` → 5.8Mbps  
- UE2: `dl_brate_kbps = 5000` → 5.0Mbps

### 3. Throughput → Priority → DSCP 조정 관계

**중요**: DSCP 값 자체가 우선순위를 결정하는 것이 아니라, **DSCP → 5QI → Priority 매핑**을 통해 우선순위가 결정됩니다. Priority 값이 **낮을수록 높은 우선순위**입니다.

```
┌─────────────────────────────────────────────────────────┐
│  제어 루프 (Control Loop) - Priority 기반               │
└─────────────────────────────────────────────────────────┘

[1단계] 스루풋 측정
  파일: lib/scheduler/logging/scheduler_metrics_handler.cpp
  ↓
  스케줄러 메트릭: dl_brate_kbps (예: 4200 kbps = 4.2Mbps)
  ↓
[2단계] 오차 계산
  파일: lib/sdap/throughput_controller.cpp
  ↓
  error = 목표 스루풋 - 현재 스루풋
  error = 5.0Mbps - 4.2Mbps = 0.8Mbps (부족)
  ↓
[3단계] PID 제어기
  파일: lib/sdap/throughput_controller.cpp
  ↓
  PID_output = Kp×error + Ki×integral + Kd×derivative
  예: PID_output = 1.0×0.8 + 0.1×2.0 + 0.01×0.1 = 1.0
  ↓
  [PID 제어기 상세 설명]
  - error: 현재 오차 = 목표 스루풋 - 현재 스루풋 (예: 5.0 - 4.2 = 0.8Mbps)
  - integral: 누적 오차 (과거 오차들의 합)
  - derivative: 오차 변화율 = 현재 오차 - 이전 오차
  - Kp (Proportional gain): 비례 게인, 오차에 즉각 반응 (기본값: 1.0)
  - Ki (Integral gain): 적분 게인, 누적 오차 보정 (기본값: 0.1)
  - Kd (Derivative gain): 미분 게인, 오버슈트 방지 (기본값: 0.01)
  ↓
[4단계] 현재 DSCP의 Priority 확인
  파일: lib/sdap/throughput_controller.cpp
  ↓
  현재 DSCP=32 → 5QI=2 → Priority=40
  ↓
[5단계] Priority 변화 계산
  파일: lib/sdap/throughput_controller.cpp
  ↓
  priority_change = round(PID_output × priority_scale)
  priority_change = round(1.0 × 10) = 10
  목표 Priority = 40 - 10 = 30 (우선순위 증가)
  (Priority가 낮을수록 높은 우선순위이므로 감소)
  ↓
[6단계] 목표 Priority에 가장 가까운 DSCP 찾기
  파일: lib/sdap/throughput_controller.cpp
  ↓
  DSCP 범위(0-63) 탐색하여 Priority=30에 가장 가까운 5QI 찾기
  예: DSCP=33 → 5QI=3 → Priority=30 (매칭!)
  ↓
[7단계] DSCP 업데이트
  파일: lib/sdap/throughput_controller.cpp
  ↓
  new_dscp = 33
  ↓
[8단계] 스케줄러 우선순위 변경
  파일: lib/scheduler/policy/scheduler_time_qos.cpp
  ↓
  DSCP=33 → 5QI=3 → Priority=30
  combined_prio = priority × ARP_priority
  낮은 combined_prio = 높은 우선순위
  ↓
[9단계] 리소스 할당 증가
  파일: lib/scheduler/policy/scheduler_time_qos.cpp
  ↓
  더 많은 PRB 할당 → 더 높은 스루풋
  ↓
[다음 주기] 새로운 스루풋 측정 (1초 후)
```

### 4. 구체적인 예시 (UE 3개)

#### 시나리오: 3개 UE, 모두 목표 5Mbps

**초기 상태 (t=0):**
```
UE0: 현재=4.2Mbps, 목표=5.0Mbps, DSCP=32
UE1: 현재=5.8Mbps, 목표=5.0Mbps, DSCP=32
UE2: 현재=5.0Mbps, 목표=5.0Mbps, DSCP=32
```

**PID 제어기 동작 (t=1초):**

**UE0 (부족한 경우):**
- error = 5.0 - 4.2 = +0.8Mbps (양수 = 부족)
- PID_output = +1.0
- 현재: DSCP=32 → 5QI=2 → Priority=40
- Priority 변화: -10 (우선순위 증가, priority 값 감소)
- 목표 Priority: 40 - 10 = 30
- 탐색 결과: DSCP=33 → 5QI=3 → Priority=30 (매칭!)
- DSCP: 32 → 33
- 결과: 스케줄러가 더 많은 리소스 할당 → 스루풋 증가 예상

**UE1 (과도한 경우):**
- error = 5.0 - 5.8 = -0.8Mbps (음수 = 과도)
- PID_output = -1.0
- 현재: DSCP=32 → 5QI=2 → Priority=40
- Priority 변화: +10 (우선순위 감소, priority 값 증가)
- 목표 Priority: 40 + 10 = 50
- 탐색 결과: DSCP=31 → 5QI=4 → Priority=50 (매칭!)
- DSCP: 32 → 31
- 결과: 스케줄러가 더 적은 리소스 할당 → 스루풋 감소 예상

**UE2 (정확한 경우):**
- error = 5.0 - 5.0 = 0.0Mbps (정확)
- PID_output = 0.0
- DSCP: 32 → 32 (변화 없음)
- 결과: 현재 상태 유지

**다음 주기 (t=2초):**
```
UE0: 현재=4.5Mbps (개선됨), 목표=5.0Mbps, DSCP=33
UE1: 현재=5.5Mbps (감소함), 목표=5.0Mbps, DSCP=31
UE2: 현재=5.0Mbps (유지), 목표=5.0Mbps, DSCP=32
```

### 5. DSCP → 5QI → Priority 매핑 (중요!)

**핵심**: DSCP 값 자체가 우선순위를 결정하는 것이 아닙니다!

```
실제 우선순위 결정 과정:
DSCP → 5QI → Priority → 스케줄러 우선순위

예시:
DSCP 63 → 5QI 69 → Priority=5   → 최고 우선순위 (낮은 priority 값)
DSCP 46 → 5QI 7  → Priority=70 → 중간 우선순위
DSCP 32 → 5QI 2  → Priority=40 → 중간 우선순위
DSCP 0  → 5QI 9  → Priority=90 → 최저 우선순위 (높은 priority 값)

중요한 점:
- Priority 값이 낮을수록 높은 우선순위 (priority=5 > priority=90)
- DSCP 값이 높다고 항상 더 높은 우선순위인 것은 아님 (매핑이 비선형적)
- 따라서 Priority 기반으로 DSCP를 조정해야 함

스케줄러에서:
combined_prio = priority × ARP_priority
낮은 combined_prio = 높은 우선순위 = 더 많은 PRB 할당
```

**Priority 기반 조정 로직:**
```
스루풋 부족 → Priority 감소 필요 → 더 낮은 Priority 값의 5QI 찾기 → 해당 DSCP 선택
스루풋 과도 → Priority 증가 필요 → 더 높은 Priority 값의 5QI 찾기 → 해당 DSCP 선택
```

### 6. 제어 루프의 지연 (Delay)

**중요한 점:**
- 현재 측정하는 스루풋은 **과거의 DSCP**의 결과입니다
- DSCP를 조정하면 → 다음 주기(1초 후)에 스루풋 변화가 나타납니다
- 이것이 PID 제어기가 필요한 이유입니다 (안정적인 제어)

```
시간축:
t=0초: DSCP=32 설정
t=1초: 스루풋 측정 (DSCP=32의 결과)
t=1초: DSCP=33으로 조정 (PID 제어기)
t=2초: 스루풋 측정 (DSCP=33의 결과)
t=2초: DSCP=34로 조정 (PID 제어기)
...
```

### 7. 코드에서의 실제 동작

**`scheduler_metrics_handler::report_metrics()`:**
```cpp
for (ue_metric_context& ue : ues) {
  // 각 UE의 현재 스루풋 계산
  scheduler_ue_metrics metrics = ue.compute_report(...);
  // metrics.dl_brate_kbps = 실제 측정된 스루풋 (kbps)
  
  // Throughput Controller에 전달
  throughput_ctrl.update_throughput(metrics);
}
```

**`throughput_controller::update_throughput()`:**
```cpp
// 1. 현재 스루풋 추출
double current_mbps = metrics.dl_brate_kbps / 1000.0;  // kbps → Mbps

// 2. 목표와 비교
double error = target_mbps - current_mbps;

// 3. PID 제어기로 DSCP 조정
uint8_t new_dscp = compute_dscp_adjustment(state, current_mbps, target_mbps);

// 4. DSCP 업데이트 (dscp_qos_mapper에 저장)
mapper.register_dscp_for_ue(ue_index, new_dscp);
```

**스케줄러에서 사용:**
```cpp
// scheduler_time_qos.cpp에서
std::optional<uint8_t> ue_dscp = mapper.get_dscp_for_ue(ue_index);
// DSCP → 5QI → Priority → 리소스 할당
```

## PID 제어기 상세 설명

### 왜 PID 제어기를 사용하는가?

**문제 상황**: 목표 스루풋(예: 5Mbps)을 달성하기 위해 단순히 오차에 비례해서 조정하면:

1. **P (Proportional)만 사용하는 경우**:
   - 오차가 0.8Mbps → 조정량 증가
   - 오차가 0.1Mbps → 조정량 감소
   - **문제**: 목표에 가까워져도 약간의 오차가 남음 (정상 상태 오차)
   - **문제**: 오버슈트 발생 가능 (목표를 넘어서거나 진동)

2. **P + I (Proportional + Integral)를 사용하는 경우**:
   - 누적 오차를 보정하여 정상 상태 오차 제거
   - **문제**: Windup 발생 가능 (오차가 계속 누적되어 과도한 조정)

3. **P + I + D (PID)를 사용하는 경우**:
   - P: 현재 오차에 즉각 반응
   - I: 누적 오차 보정 (정상 상태 오차 제거)
   - D: 오차 변화율에 반응 (오버슈트 방지)
   - **결과**: 안정적이고 정확한 제어

**실제 예시**:
```
단순 비례 제어:
- 목표: 5Mbps, 현재: 4.2Mbps → 조정
- 목표: 5Mbps, 현재: 4.9Mbps → 작은 조정
- 목표: 5Mbps, 현재: 4.95Mbps → 매우 작은 조정
→ 결국 4.95Mbps에서 멈춤 (정상 상태 오차)

PID 제어:
- 목표: 5Mbps, 현재: 4.2Mbps → P+I+D로 조정
- 목표: 5Mbps, 현재: 4.9Mbps → I가 누적 오차 보정
- 목표: 5Mbps, 현재: 4.95Mbps → I가 계속 보정
→ 결국 5.0Mbps에 정확히 도달
```

PID 제어기는 이 문제들을 해결하기 위해 3가지 요소를 결합합니다.

### PID 계산식

```
PID_output = Kp × error + Ki × integral + Kd × derivative
```

### 각 항목의 의미

#### 1. **P (Proportional) - 비례 항**
```
P_term = Kp × error
```

**의미**: 현재 오차에 비례하여 즉각 반응
- **error**: 목표 스루풋 - 현재 스루풋
- **Kp (Proportional gain)**: 비례 게인 (기본값: 1.0)
- **역할**: 오차가 클수록 더 큰 조정

**예시**:
- error = 0.8Mbps (부족) → P_term = 1.0 × 0.8 = 0.8
- error = 0.2Mbps (부족) → P_term = 1.0 × 0.2 = 0.2

**문제점**: 
- 정상 상태 오차가 남을 수 있음
- 오버슈트 가능 (너무 빠른 반응)

#### 2. **I (Integral) - 적분 항**
```
I_term = Ki × integral
integral = integral + error  (누적)
```

**의미**: 과거 오차들의 누적 합을 보정
- **integral**: 누적 오차 (과거 오차들의 합)
- **Ki (Integral gain)**: 적분 게인 (기본값: 0.1)
- **역할**: 정상 상태 오차 제거, 지속적인 오차 보정

**예시**:
- 1초차: error = 0.8 → integral = 0.8
- 2초차: error = 0.5 → integral = 0.8 + 0.5 = 1.3
- 3초차: error = 0.2 → integral = 1.3 + 0.2 = 1.5
- I_term = 0.1 × 1.5 = 0.15

**문제점**:
- **Windup**: 오차가 계속 누적되어 과도한 조정
- **해결**: Anti-windup (integral을 -100 ~ +100으로 제한)

#### 3. **D (Derivative) - 미분 항**
```
D_term = Kd × (error - last_error)
```

**의미**: 오차의 변화율에 반응하여 오버슈트 방지
- **derivative**: 오차 변화율 = 현재 오차 - 이전 오차
- **Kd (Derivative gain)**: 미분 게인 (기본값: 0.01)
- **역할**: 오버슈트 방지, 안정성 향상

**예시**:
- 이전 오차: 1.0Mbps
- 현재 오차: 0.8Mbps
- derivative = 0.8 - 1.0 = -0.2 (개선 중)
- D_term = 0.01 × (-0.2) = -0.002 (약간의 제동)

**문제점**:
- 노이즈에 민감할 수 있음
- 따라서 Kd 값이 작게 설정됨 (0.01)

### 실제 계산 예시

**시나리오**: 목표 5Mbps, 현재 4.2Mbps

```
[초기 상태]
- error = 5.0 - 4.2 = 0.8Mbps
- integral = 0.0 (초기)
- last_error = 0.0 (초기)

[1초차 계산]
- P_term = 1.0 × 0.8 = 0.8
- integral = 0.0 + 0.8 = 0.8
- I_term = 0.1 × 0.8 = 0.08
- derivative = 0.8 - 0.0 = 0.8
- D_term = 0.01 × 0.8 = 0.008
- PID_output = 0.8 + 0.08 + 0.008 = 0.888

[2초차 계산 (스루풋이 4.5Mbps로 개선)]
- error = 5.0 - 4.5 = 0.5Mbps
- P_term = 1.0 × 0.5 = 0.5
- integral = 0.8 + 0.5 = 1.3
- I_term = 0.1 × 1.3 = 0.13
- derivative = 0.5 - 0.8 = -0.3 (개선 중)
- D_term = 0.01 × (-0.3) = -0.003
- PID_output = 0.5 + 0.13 - 0.003 = 0.627
```

### 게인 값 튜닝 가이드

**기본 설정** (안정적인 제어):
- Kp = 1.0
- Ki = 0.1
- Kd = 0.01

**빠른 응답이 필요한 경우**:
- Kp = 2.0 (더 큰 즉각 반응)
- Ki = 0.2 (더 빠른 누적 보정)
- Kd = 0.02 (더 강한 오버슈트 방지)

**안정적인 제어가 중요한 경우**:
- Kp = 0.5 (더 작은 즉각 반응)
- Ki = 0.05 (더 느린 누적 보정)
- Kd = 0.005 (더 약한 오버슈트 방지)

## 사용 방법

### 기본 설정

```cpp
#include "srsran/sdap/throughput_controller.h"

// 싱글톤 인스턴스 가져오기
auto& controller = throughput_controller::get_instance();

// 기본 설정
throughput_controller::config cfg;
cfg.target_throughput_mbps = 5.0;  // 목표 스루풋: 5Mbps
cfg.kp = 1.0;   // Proportional gain
cfg.ki = 0.1;   // Integral gain
cfg.kd = 0.01;  // Derivative gain
cfg.control_period_ms = std::chrono::milliseconds(1000);  // 1초마다 조절
cfg.min_dscp = 0;
cfg.max_dscp = 63;
cfg.initial_dscp = 32;
cfg.enabled = true;

controller.set_config(cfg);
```

### UE별 목표 스루풋 설정

UE별 목표 스루풋을 매핑으로 한 번에 설정합니다:

```cpp
// 여러 UE의 목표 스루풋을 한 번에 설정
std::unordered_map<du_ue_index_t, double> ue_target_map = {
  {0, 5.0},   // UE0: 5Mbps
  {1, 10.0},  // UE1: 10Mbps
  {2, 3.0}    // UE2: 3Mbps
};

controller.set_target_throughput_map(ue_target_map);
```

**동작 방식:**
- 이미 존재하는 UE는 즉시 목표 스루풋이 업데이트됨
- 아직 등록되지 않은 UE는 등록 시 자동으로 해당 목표 스루풋이 적용됨

**참고**: 특정 UE만 개별적으로 변경하려면 `set_target_throughput(ue_index, target_mbps)` 함수를 사용할 수 있습니다.

### 초기화 시점 설정 (DU 시작 시)

**파일 위치**: `apps/du/du.cpp` (412-420줄)

```cpp
// Initialize throughput controller with UE-specific target throughput mapping
auto& throughput_ctrl = throughput_controller::get_instance();
std::unordered_map<du_ue_index_t, double> ue_target_throughput_map = {
    {0, 5.0},   // UE0: 5Mbps
    {1, 10.0},  // UE1: 10Mbps
    {2, 3.0}    // UE2: 3Mbps
};
throughput_ctrl.set_target_throughput_map(ue_target_throughput_map);
du_logger.info("Throughput controller initialized with UE-specific target throughput mapping");
```

**왜 이 위치인가?**

1. **DU 인스턴스 생성 후**: `du_inst`가 생성된 후에 설정해야 스케줄러와 관련 컴포넌트가 준비됨
2. **DU 시작 전**: `du_inst.get_operation_controller().start()` 전에 설정해야 UE가 등록될 때 자동으로 적용됨
3. **명령어 파서 등록 후**: 모든 초기화가 완료된 시점이므로 안전함
4. **싱글톤 패턴**: `throughput_controller`는 싱글톤이므로 어디서든 호출 가능하지만, 초기화 시점에 설정하는 것이 가장 안전하고 명확함

**코드 흐름:**
```
1. DU 인스턴스 생성 (du_inst)
2. 명령어 파서 등록
3. → 여기서 throughput controller 초기화 ←
4. DU 시작 (du_inst.get_operation_controller().start())
5. UE 등록 시 자동으로 설정된 목표 스루풋 적용
```

### 제어 활성화/비활성화

```cpp
// 특정 UE에 대해 제어 활성화
controller.enable_control(ue_index, true);

// 제어 비활성화
controller.enable_control(ue_index, false);
```

### 현재 DSCP 값 조회

```cpp
// 제어된 DSCP 값 조회
auto dscp = controller.get_controlled_dscp(ue_index);
if (dscp.has_value()) {
    // 제어가 활성화된 경우 DSCP 값 사용
    uint8_t current_dscp = dscp.value();
}
```

### 로깅

스루풋 제어기는 `THROUGHPUT_CTRL` 로거를 사용합니다:

```
[THROUGHPUT_CTRL] Target throughput set for UE0: 5Mbps
[THROUGHPUT_CTRL] DSCP adjusted for UE0: throughput=4.2Mbps (target=5.0Mbps), DSCP=32->33
```

## 요약

1. **각 UE는 독립적으로 제어됨**: UE0, UE1, UE2 각각 독립적인 PID 상태
2. **Throughput = 실제 측정된 스루풋**: `dl_brate_kbps` (kbps 단위)
3. **Priority 기반 조정**: DSCP가 아닌 Priority 값을 기준으로 조정
4. **DSCP 조정 방향**:
   - 스루풋 부족 → Priority 감소 → 우선순위 증가 → 리소스 증가
   - 스루풋 과도 → Priority 증가 → 우선순위 감소 → 리소스 감소
5. **제어 주기**: 1초마다 측정 및 조정 (설정 가능)
6. **지연**: DSCP 변경 → 1초 후 스루풋 변화 반영
7. **PID 제어기**: P(비례) + I(적분) + D(미분)로 안정적이고 정확한 제어
8. **자동 통합**: 스케줄러 메트릭 핸들러에 자동으로 통합되어 별도 호출 불필요

