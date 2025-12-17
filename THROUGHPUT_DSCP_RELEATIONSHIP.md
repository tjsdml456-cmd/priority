# UE별로 목표 스루풋을 고정해 두고, 그 목표 스루풋에 수렴하도록 DSCP(→ Priority)를 PID 제어로 동적으로 조정하는 구조설명


### 1. Throughput이란구하는 법

**`scheduler_ue_metrics.dl_brate_kbps`**: 
- 각 UE가 **실제로 받은** 다운링크 비트레이트 (kbps 단위)
- 스케줄러가 할당한 리소스로 전송된 데이터의 실제 스루풋
- 주기적으로 측정됨 (예: 1초마다)

#### 스루풋 계산 방법

**코드 위치**: `lib/scheduler/logging/scheduler_metrics_handler.cpp:643, 246-254`

스루풋은 다음과 같이 계산됩니다:

```cpp
// 성공적으로 전송된 Transport Block 바이트 수를 누적
void handle_dl_harq_ack(du_ue_index_t ue_index, bool ack, units::bytes tbs) {
  if (ack) {  // ACK를 받은 경우만 누적
    u.data.sum_dl_tb_bytes += tbs.value();
  }
}

// 리포트 기간 동안의 평균 스루풋 계산
dl_brate_kbps = (sum_dl_tb_bytes * 8) / metric_report_period.count();
// sum_dl_tb_bytes: 누적된 바이트 수
// * 8: 바이트 → 비트 변환
// metric_report_period.count(): 리포트 기간 (밀리초, 예: 1000ms)
```


#### 최대 스루풋과 TBS의 관계

**핵심**: 스루풋 = TBS의 초당 양입니다. 최대 스루풋은 TBS size를 결정하는 요소들에 의해 결정됩니다.

**스루풋과 TBS의 관계:**
```
스루풋 (kbps) = (성공한 TBS들의 총 바이트 수 × 8) / 리포트 기간(밀리초)
             = TBS 크기 × 초당 전송 횟수
```

**TBS (Transport Block Size) 결정 요소:**

TBS size는 다음 요소들에 의해 결정됩니다 (`lib/ran/sch/tbs_calculator.cpp:124-143`):

1. **PRB 수 (Physical Resource Blocks)**
   - 대역폭에 의해 결정: 10MHz = 52 PRB, 20MHz = 106 PRB, 100MHz = 273 PRB (SCS 30kHz 기준)
   - PRB 수가 많을수록 TBS가 증가

2. **MCS (Modulation and Coding Scheme)**
   - 변조 방식: QAM64 (6 bits/symbol), QAM256 (8 bits/symbol)
   - Code Rate: MCS가 높을수록 더 높은 code rate (더 많은 데이터)
   - 채널 상태(CQI)에 따라 자동 선택

3. **레이어 수 (Layers)**
   - MIMO 구성: 1x1 (1 layer), 2x2 (2 layers), 4x4 (4 layers) 등
   - 레이어 수가 많을수록 TBS가 증가 (거의 비례)

4. **심볼 수 (Symbols per slot)**
   - 일반적으로 14 symbols per slot (PDSCH)
   - DMRS, 오버헤드 제외하면 실제 데이터 심볼 수 감소

**TBS 계산 공식** (`tbs_calculator.cpp:124-143`):
```cpp
// Step 1: RE (Resource Element) 수 계산
// NOF_SUBCARRIERS_PER_RB = 12 (리소스 블록당 서브캐리어 수)
nof_re_prime = NOF_SUBCARRIERS_PER_RB × nof_symb_sh - nof_dmrs_prb - nof_oh_prb
              = 12 × symbols_per_slot - DMRS_per_PRB - overhead_per_PRB
nof_re = min(156, nof_re_prime) × n_prb
// n_prb (PRB_count): PRB 수 (대역폭에 의해 결정)
// nof_symb_sh (symbols_per_slot): 일반적으로 14 (PDSCH)
// nof_dmrs_prb, nof_oh_prb: DMRS 및 오버헤드

// Step 2: TBS (bits) 계산
nof_info = scaling × nof_re × tcr × modulation_level × nof_layers
         = scaling × nof_re × code_rate × bits_per_symbol × layers
// scaling: TB scaling factor (일반적으로 1.0, tb_scaling_field에 의해 결정)
// tcr (code_rate): MCS에 포함된 정규화된 code rate (0.0 ~ 1.0)
// modulation_level (bits_per_symbol): 변조 방식 (QAM64=6, QAM256=8)
// nof_layers (layers): MIMO 레이어 수 (1, 2, 4, ...)

// Step 3: 표준 테이블에 따라 TBS 양자화
if (nof_info <= 3824) {
  TBS_bits = tbs_calculator_step3(nof_info)  // 표준 테이블 조회
} else {
  TBS_bits = tbs_calculator_step4(nof_info, tcr)  // 복잡한 양자화 로직
}

// Step 4: TBS (bytes) 변환
TBS_bytes = TBS_bits / 8
```

**결론**: 
- **스루풋 = TBS/초** (TBS 크기 × 초당 전송 횟수)
- **TBS size**는 **PRB 수, MCS(변조 방식 + code rate), 레이어 수** 등에 의해 결정
- **최대 스루풋** = 최대 TBS × 초당 최대 전송 횟수 (대역폭, 변조, 레이어, TDD/FDD 등에 의해 결정)

**최대 스루풋 계산:**

최대 스루풋을 결정하는 요인들:

1. **대역폭 → PRB 수**
   - 대역폭이 클수록 PRB 수 증가 → TBS 증가 → 스루풋 증가

2. **변조 방식 (MCS의 일부)**
   - QAM64: 6 bits/symbol
   - QAM256: 8 bits/symbol (더 높은 TBS 가능)

3. **MIMO 레이어 수**
   - 1x1 (SISO): 1개 레이어
   - 2x2 MIMO: 2개 레이어 (TBS 약 2배)
   - 4x4 MIMO: 4개 레이어 (TBS 약 4배)
   - 최대 8개 레이어까지 지원

4. **TDD/FDD 설정**
   - FDD: DL/UL 동시 전송 가능 (더 높은 DL 스루풋)
   - TDD: DL/UL 슬롯 비율에 따라 달라짐 (예: DL 70%, UL 30%)
   - TDD에서는 DL 슬롯이 많을수록 초당 전송 기회 증가 → 스루풋 증가

5. **채널 상태 (CQI → MCS)**
   - 채널 상태가 좋을수록 높은 MCS 사용 가능
   - 높은 MCS = 더 높은 변조 + 더 높은 code rate → 더 큰 TBS

**이론적 최대 스루풋 계산 예시 (20MHz, SCS 30kHz, QAM256, 4x4 MIMO, FDD):**
```
- PRB 수: 106개
- RE per PRB: ~156 (DMRS, 오버헤드 제외)
- Symbol per slot: 14
- Slot per ms: 2 (30kHz SCS)
- Bits per symbol: 8 (QAM256)
- Layers: 4
- 이론적 최대 ≈ 1.48 Gbps
- 실제로는 오버헤드, 채널 상태, 스케줄링 등으로 인해 더 낮음
```

**실제 환경 예상값:**
- 20MHz, QAM256, 2x2 MIMO, FDD: 약 200-300 Mbps
- 10MHz, QAM64, 1x1, FDD: 약 30-50 Mbps
- Throughput Controller의 목표 스루풋 (1.5Mbps, 2.0Mbps)은 이러한 최대 스루풋보다 훨씬 낮게 설정되어 있습니다.

#### 스루풋 제어 메커니즘: 목표 스루풋 → Priority → PRB → TBS → 스루풋

**핵심 개념:**

스루풋 제어는 다음 경로로 동작합니다:

```
1. 목표 스루풋 설정 (예: UE0=1.5Mbps, UE1=2.0Mbps, UE2=1.0Mbps)
   ↓
2. 실제 스루풋 측정 (dl_brate_kbps = TBS들의 초당 총 바이트 수)
   ↓
3. PID 제어기로 Priority 계산 (target_priority)
   ↓
4. 스케줄러에서 Priority 적용 (runtime_qos.priority)
   ↓
5. final_priority 계산 (pf_weight × gbr_weight × prio_weight × delay_weight)
   ↓
6. 스케줄러가 final_priority로 UE 선택 및 PRB 할당
   ↓
7. PRB 수에 따라 TBS 결정 (TBS = f(PRB, MCS, 레이어, ...))
   ↓
8. TBS가 증가하면 스루풋 증가
```

**중요한 질문들:**

**Q1: 왜 du.cpp와 gnb.cpp에서 UE 인덱스당 스루풋을 설정했는가?**

**A:** UE가 등록되기 **전에** 목표 스루풋을 미리 설정하기 위함입니다.

- **코드 위치**: `du.cpp:412-419`, `gnb.cpp:550-557`
- **설정 시점**: `du_inst.get_operation_controller().start()` **전에** 설정
- **이유**: 
  - `throughput_controller.cpp:94-100`에서 UE 초기화 시점에 확인
  - UE가 등록될 때 `update_throughput()`이 호출되면, 미리 설정된 `ue_target_throughput_map`에서 해당 UE의 목표 스루풋을 찾아 자동 적용
  - 만약 UE 등록 후에 설정하면, 이미 초기화된 UE는 기본값을 사용하게 됨

```cpp
// UE 초기화 시 (throughput_controller.cpp:94-100)
auto preconfigured_it = ue_target_throughput_map.find(metrics.ue_index);
if (preconfigured_it != ue_target_throughput_map.end()) {
  new_state.target_mbps = preconfigured_it->second;  // 미리 설정된 값 사용
} else {
  new_state.target_mbps = cfg.target_throughput_mbps;  // 기본값 사용
}
```

**Q2: UE당 스루풋을 설정했으면 무엇을 바꿔야 하는가?**

**A:** **Priority**를 변경해야 합니다.

- 목표 스루풋을 설정하면 → PID 제어기가 `target_priority`를 계산
- 이 `target_priority`가 스케줄러의 `runtime_qos.priority`에 적용됨 (`scheduler_time_qos.cpp:395-409`)
- Priority가 변경되면 → `final_priority`가 변경됨 (`scheduler_time_qos.cpp:252`)
- `final_priority`가 높은 UE가 스케줄러에서 우선 선택되어 더 많은 PRB 할당

**Q3: 스루풋 = TBS의 초당 갯수?**

**A:** 맞습니다. 정확히는:

```
스루풋 (kbps) = (성공한 TBS들의 총 바이트 수 × 8) / 리포트 기간(밀리초)
```

- **코드 위치**: `scheduler_metrics_handler.cpp:643, 246-254`
- `sum_dl_tb_bytes`: HARQ ACK를 받은 TBS들의 총 바이트 수
- 리포트 기간 동안 누적된 값 (예: 1000ms = 1초)
- **예시**: 1초 동안 150,000 bytes의 TBS가 성공 → `(150,000 × 8) / 1000 = 1,200 kbps = 1.2 Mbps`

**Q4: DSCP로 PRB를 직접 조절하는 것인가?**

**A:** 직접 조절이 아니라 **간접적으로** 조절합니다:

```
DSCP 변경
  ↓
Priority 변경 (DSCP → 5QI → Priority, 또는 Throughput Controller가 직접 설정)
  ↓
final_priority 변경 (스케줄러에서 계산)
  ↓
스케줄러가 final_priority 높은 UE를 우선 선택
  ↓
우선 선택된 UE가 더 많은 PRB 할당받음
```

- DSCP 자체가 PRB 수를 직접 제어하는 것이 아님
- Priority → final_priority → 스케줄링 우선순위를 통해 PRB 할당에 영향을 줌

**Q5: QoS를 이용한 PRB 조절로 TBS의 초당 갯수를 조절하는 것이 가능한가?**

**A:** **가능합니다.**

**코드 흐름:**

1. **Priority → final_priority** (`scheduler_time_qos.cpp:252`)
   ```cpp
   final_priority = combine_qos_metrics(pf_weight, gbr_weight, prio_weight, delay_weight);
   ```

2. **final_priority → UE 선택** (`intra_slice_scheduler.cpp:463`)
   - 스케줄러가 `final_priority`가 높은 UE를 우선 선택
   - Priority queue에서 높은 priority를 가진 UE가 먼저 스케줄링됨

3. **PRB 할당 → TBS 계산** (`tbs_calculator.cpp:124-143`)
   ```cpp
   TBS = f(PRB 수, MCS, 레이어 수, 심볼 수, ...)
   ```
   - PRB 수가 증가하면 TBS가 증가
   - MCS가 높으면 (더 높은 변조 방식) TBS가 증가

4. **TBS → 스루풋**
   - PRB ↑ → TBS ↑ → 스루풋 ↑
   - 더 많은 PRB를 할당받으면 더 큰 TBS를 전송할 수 있고, 결과적으로 스루풋이 증가

**결론:**

- ✅ **가능**: Priority → final_priority → PRB 할당 → TBS → 스루풋 경로로 제어 가능
- ⚠️ **제한사항**: 
  - 채널 상태(MCS 변화), 다른 UE의 경쟁, 스케줄링 제약 등이 결과에 영향을 줄 수 있음
  - 완벽하게 정확한 스루풋을 보장하지는 못함 (PID 제어기로 목표값에 근사)

### 3. Throughput → Priority → DSCP 조정 관계

**중요**: DSCP 값 자체가 우선순위를 결정하는 것이 아니라, **DSCP → 5QI → Priority 매핑**을 통해 우선순위가 결정됩니다. Priority 값이 **낮을수록 높은 우선순위**입니다.

```
┌─────────────────────────────────────────────────────────┐
│  제어 루프 (Control Loop) - Priority 기반               │
└─────────────────────────────────────────────────────────┘

[0단계] 초기화: UE별 목표 스루풋 설정
  파일: apps/du/du.cpp 또는 apps/gnb/gnb.cpp
  ↓
  UE 등록 전에 목표 스루풋 미리 설정
  예: UE0=1.5Mbps, UE1=2.0Mbps, UE2=1.0Mbps
  ↓
  throughput_ctrl.set_target_throughput_map(ue_target_throughput_map)
  ↓
  (UE가 등록되면 자동으로 해당 목표 스루풋 적용)
  ↓
[1단계] 스루풋 측정
  파일: lib/scheduler/logging/scheduler_metrics_handler.cpp
  ↓
  스케줄러 메트릭: dl_brate_kbps (예: 1200 kbps = 1.2Mbps)
  ↓
[2단계] 오차 계산
  파일: lib/sdap/throughput_controller.cpp
  ↓
  error = 목표 스루풋 - 현재 스루풋
  error = 1.5Mbps - 1.2Mbps = 0.3Mbps (부족)
  ↓
[3단계] PID 제어기
  파일: lib/sdap/throughput_controller.cpp
  ↓
  PID_output = Kp×error + Ki×integral + Kd×derivative
  예: PID_output = 1.0×0.3 + 0.1×0.5 + 0.01×0.05 = 0.35
  ↓
  [PID 제어기 상세 설명]
  - error: 현재 오차 = 목표 스루풋 - 현재 스루풋 (예: 1.5 - 1.2 = 0.3Mbps)
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
  priority_change = round(0.35 × 10) = 4
  목표 Priority = 40 - 4 = 36 (우선순위 증가)
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
  Throughput Controller가 계산한 target_priority=30을 스케줄러에서 직접 사용
  (DSCP → 5QI → Priority 경로 우회)
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

**참고**: 아래 예시는 PID 제어기의 작동 방식을 설명하기 위한 예시입니다. 실제 코드에서 사용하는 값과는 다를 수 있습니다 (실제 코드 예시: UE0=1.5Mbps, UE1=2.0Mbps, UE2=1.0Mbps).

#### 시나리오: 3개 UE, 모두 목표 5Mbps (예시)

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
cfg.target_throughput_mbps = 1.5;  // 목표 스루풋: 1.5Mbps
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
  {to_du_ue_index(0), 1.5},  // UE0: 1.5Mbps
  {to_du_ue_index(1), 2.0},  // UE1: 2.0Mbps
  {to_du_ue_index(2), 1.0}   // UE2: 1.0Mbps
};

controller.set_target_throughput_map(ue_target_map);
```

**동작 방식:**
- 이미 존재하는 UE는 즉시 목표 스루풋이 업데이트됨
- 아직 등록되지 않은 UE는 등록 시 자동으로 해당 목표 스루풋이 적용됨

**참고**: 특정 UE만 개별적으로 변경하려면 `set_target_throughput(ue_index, target_mbps)` 함수를 사용할 수 있습니다.

### 초기화 시점 설정 (DU/gNB 시작 시)

**파일 위치**: 
- `apps/du/du.cpp` (412-419줄)
- `apps/gnb/gnb.cpp` (550-558줄)

```cpp
// Initialize throughput controller with UE-specific target throughput mapping
auto& throughput_ctrl = throughput_controller::get_instance();
std::unordered_map<du_ue_index_t, double> ue_target_throughput_map = {
    {to_du_ue_index(0), 1.5},  // UE0: 1.5Mbps
    {to_du_ue_index(1), 2.0},  // UE1: 2.0Mbps
    {to_du_ue_index(2), 1.0}   // UE2: 1.0Mbps
};
throughput_ctrl.set_target_throughput_map(ue_target_throughput_map);
// du_logger.info() 또는 gnb_logger.info() 사용
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

스루풋 제어기는 `THROUGHPUT_CTRL` 로거를 사용합니다 (로그는 파일에만 기록되고 콘솔에는 출력되지 않음):

```
[THROUGHPUT_CTRL] Target throughput set for UE0: 1.5Mbps
[THROUGHPUT_CTRL] DSCP adjusted for UE0: throughput=1.2Mbps (target=1.5Mbps), DSCP=32->33
[THROUGHPUT_CTRL] PID control (Priority-based): current=1.2Mbps, target=1.5Mbps, error=0.3, ...
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

---

## 최근 변경 사항 및 문제점 분석 (2025-12-16)

### 주요 변경 사항

#### 1. Throughput Controller가 계산한 Priority 값을 스케줄러에 직접 적용
**변경 파일**: `lib/scheduler/policy/scheduler_time_qos.cpp`

**변경 내용**:
- **이전**: DSCP → 5QI → Priority 경로로 우회하여 Priority 값 제한 (예: Priority=90으로 제한)
- **현재**: Throughput Controller가 계산한 `target_priority` 값을 스케줄러에서 직접 사용
- `#include "srsran/sdap/throughput_controller.h"` 추가 (29줄)

**변경된 코드 흐름**:
```cpp
// 이전 방식 (문제 있었음):
DSCP → 5QI 매핑 → Priority (최대 90으로 제한)
→ combined_prio = Priority × ARP = 90 × 8 = 720
→ prio_weight = (1906 - 720) / 1906 = 0.622

// 현재 방식 (개선됨):
Throughput Controller → target_priority 계산 (예: 127)
→ 스케줄러가 Priority=127 직접 사용
→ combined_prio = Priority × ARP = 127 × 8 = 1016
→ prio_weight = (1906 - 1016) / 1906 = 0.467
```

**코드 위치**: `lib/scheduler/policy/scheduler_time_qos.cpp:379-409`
```cpp
auto& throughput_ctrl = throughput_controller::get_instance();
std::optional<uint8_t> target_priority = throughput_ctrl.get_target_priority(ue_index);

if (target_priority.has_value()) {
  // Throughput Controller가 활성화되어 있으면 계산된 target_priority 직접 사용
  effective_priority = qos_prio_level_t{target_priority.value()};
  logger.info("[STEP6-SCHED] Throughput Controller Priority 적용 - UE{} LCID{} Priority={}",
              ue_index, static_cast<unsigned>(lc->lcid), target_priority.value());
} else {
  // Throughput Controller가 비활성화되어 있으면 DSCP 기반 Priority 사용
  const standardized_qos_characteristics* qos_chars = get_5qi_to_qos_characteristics_mapping(effective_5qi);
  if (qos_chars != nullptr) {
    effective_priority = qos_chars->priority;
  }
}
```

**효과**:
- DSCP → 5QI → Priority 경로를 우회하여 Throughput Controller가 계산한 Priority를 직접 적용
- Priority 범위를 1-127까지 전체 사용 가능 (이전에는 5QI 매핑으로 인해 Priority=90 정도로 제한됨)

#### 2. Throughput Controller에 `target_priority` 저장 및 조회 기능 추가
**변경 파일**: 
- `include/srsran/sdap/throughput_controller.h`
- `lib/sdap/throughput_controller.cpp`

**변경 내용**:
- `pid_state` 구조체에 `target_priority` 필드 추가
- `get_target_priority()` 함수 추가: 스케줄러에서 Priority 값 조회 가능
- `compute_dscp_adjustment()`에서 계산한 `target_priority`를 상태에 저장

**코드 위치**: `lib/sdap/throughput_controller.cpp:294-296`
```cpp
int target_priority = static_cast<int>(current_priority) - priority_change;
target_priority = std::clamp(target_priority, 1, 127); // Priority 범위 제한
state.target_priority = static_cast<uint8_t>(target_priority);  // 상태에 저장
```

#### 3. SDAP에서 iperf3 DSCP 무시 로직 추가
**변경 파일**: `lib/sdap/sdap_entity_tx_impl.h`

**변경 내용**:
- Throughput Controller가 활성화된 UE의 경우, iperf3가 보낸 DSCP 값을 무시
- Throughput Controller가 계산한 DSCP가 iperf3 DSCP에 의해 덮어쓰이지 않도록 보호

**코드 위치**: `lib/sdap/sdap_entity_tx_impl.h:104-123`
```cpp
auto& mapper = dscp_qos_mapper::get_instance();
auto& throughput_ctrl = throughput_controller::get_instance();

// Throughput controller가 활성화되어 있으면 iperf3의 DSCP는 무시하고
// throughput controller가 계산한 DSCP를 사용
bool ctrl_enabled = throughput_ctrl.is_control_enabled(static_cast<du_ue_index_t>(ue_index));
if (not ctrl_enabled) {
  mapper.register_dscp_for_ue(ue_index, dscp.value());
  logger.log_info("[STEP2-MAPPER] DSCP 등록 완료 - UE={} DSCP={} (throughput control 비활성화)",
                  ue_index, dscp.value());
} else {
  // Throughput controller가 활성화되어 있으면 iperf3의 DSCP를 무시
  logger.log_info("[STEP2-MAPPER] DSCP 등록 건너뜀 - UE={} DSCP={} (throughput controller 활성화, iperf3 DSCP 무시)",
                   ue_index, dscp.value());
}
```

**이유**:
- iperf3가 패킷에 설정한 DSCP 값이 Throughput Controller가 계산한 DSCP 값을 덮어쓰는 문제 발생
- Throughput Controller가 목표 스루풋에 맞춰 동적으로 DSCP를 조정하는데, iperf3의 고정 DSCP가 이를 방해
- 따라서 Throughput Controller가 활성화된 경우 iperf3 DSCP를 무시하여 제어 루프가 정상 동작하도록 수정

#### 4. 로거 Lazy Initialization 적용
**변경 파일**: `lib/sdap/throughput_controller.cpp`

**변경 내용**:
- `THROUGHPUT_CTRL` 로거를 lazy initialization으로 변경
- `srslog::init()` 후에 로거를 가져와야 올바른 sink(파일)를 사용함

**코드 위치**: `lib/sdap/throughput_controller.cpp:32-38`
```cpp
// 이전 방식 (문제 있었음):
// static srslog::basic_logger& logger = srslog::fetch_basic_logger("THROUGHPUT_CTRL", false);
// → srslog::init() 전에 초기화되어 콘솔로 출력됨

// 현재 방식 (수정됨):
static srslog::basic_logger& get_logger()
{
  static srslog::basic_logger& logger = srslog::fetch_basic_logger("THROUGHPUT_CTRL");
  return logger;
}
// → srslog::init() 후에 로거를 가져와서 파일로 출력됨
```

**이유**:
- 로거가 정적 변수로 초기화되면 `srslog::init()` 전에 실행되어 기본 sink(콘솔)를 사용
- 함수 내부에서 정적 변수로 선언하면 함수 호출 시점에 초기화되어, `srslog::init()` 이후에 로거를 가져올 수 있음
- 이를 통해 로그가 파일(`gnb.log`)에만 기록되고 콘솔에는 출력되지 않음

#### 5. 로깅 개선
**변경 파일**: `lib/sdap/throughput_controller.cpp`

**변경 내용**:
- DSCP 변경 전 값을 저장하여 로그에 올바르게 표시
- `best_priority`와 `priority_diff` 로깅 추가: 실제 선택된 DSCP의 Priority와 차이 확인

**코드 위치**: `lib/sdap/throughput_controller.cpp:130-148, 300-308, 316-324`
```cpp
// DSCP 변경 전 값 저장
uint8_t old_dscp = state.current_dscp;
uint8_t new_dscp = compute_dscp_adjustment(state, current_mbps, state.target_mbps);

if (new_dscp != state.current_dscp) {
  // ... DSCP 업데이트 ...
  get_logger().info("DSCP adjusted for UE{}: throughput={:.2f}Mbps (target={:.2f}Mbps), DSCP={}->{}",
                    ue_index, current_mbps, state.target_mbps, old_dscp, new_dscp);
}

// PID 제어 로그에 best_priority, priority_diff 추가
get_logger().info("PID control (Priority-based): current={:.2f}Mbps, target={:.2f}Mbps, ... "
                  "best_dscp={}, best_priority={}, priority_diff={} [CHANGED]",
                  ..., best_dscp, best_priority, priority_diff);
```

### 현재 문제점 및 원인 분석

#### 문제 1: Priority 변경은 되지만 스루풋이 목표에 수렴하지 않음

**현상**:
- Throughput Controller가 `target_priority=127` (최저 우선순위)로 계산
- 스케줄러가 Priority=127을 적용하여 `combined_prio=1016`, `prio_weight=0.467`로 변경
- 하지만 실제 스루풋은 여전히 높음:
  - UE0: `current=14.44Mbps, target=1.50Mbps` (error=-12.94Mbps)
  - UE1: `current=14.38Mbps, target=2.00Mbps` (error=-12.38Mbps)

**로그 확인**:
```
[THROUGHPUT_CTRL] PID control: current=14.44Mbps, target=1.50Mbps, error=-12.94
[THROUGHPUT_CTRL] target_priority=127, best_dscp=0, best_priority=90
[SCHED] Throughput Controller Priority 적용 - Priority=127
[SCHED] DL Priority calc: min_combined_prio=1016, prio_weight=0.467
[SCHED] DL Final Priority: final_priority=0.001
```

**가능한 원인들**:

1. **PF Weight가 매우 작아서 Priority의 영향이 미미함**
   - `pf_weight = 0.001` (매우 작음)
   - `final_priority = pf_weight × gbr_weight × prio_weight × delay_weight`
   - `final_priority = 0.001 × 1.0 × 0.467 × 1.0 = 0.001`
   - PF weight가 작으면 Priority 변경의 영향이 제한적일 수 있음

2. **제어 지연 (Control Delay)**
   - Priority 변경 → 스케줄러 반영 → 리소스 할당 변경 → 스루풋 측정까지 시간 소요
   - PID 제어기는 적분 항(I)으로 누적 오차를 보정하지만, 충분한 시간이 필요할 수 있음

3. **Priority 범위 제한**
   - `target_priority=127`로 계산되었지만, 이미 Priority 범위의 최대값
   - 더 낮은 우선순위(더 높은 Priority 값)로 갈 수 없음
   - 현재 모든 UE가 Priority=127을 사용하여 차별화 부족

4. **다른 요인의 영향**
   - iperf3가 높은 비트레이트(예: 5Mbps)로 전송 중일 수 있음
   - 채널 상태나 다른 스케줄링 요인들이 Priority보다 더 큰 영향

#### 문제 2: DSCP 매핑의 한계

**현상**:
- `target_priority=127`로 계산되었지만, DSCP 매핑 테이블에는 Priority=127에 해당하는 DSCP가 없음
- 결과적으로 `best_dscp=0` (Priority=90)이 선택됨
- 하지만 스케줄러는 `target_priority=127`을 직접 사용하므로 이 문제는 해결됨

**로그 확인**:
```
[THROUGHPUT_CTRL] target_priority=127, best_dscp=0, best_priority=90, priority_diff=0
```

#### 문제 3: 모든 UE가 동일한 Priority를 가지는 경우

**현상**:
- 모든 UE가 Priority=127을 사용하면 `combined_prio`도 동일함
- `prio_weight`가 동일하여 Priority 기반 차별화 불가
- 다른 UE와의 상대적 우선순위 차이가 없음

### 해결 방향

#### 1. Priority 차별화
- UE별로 다른 Priority 값을 사용하여 상대적 우선순위 차이 확보
- 현재는 모든 UE가 Priority=127 (최저 우선순위)로 동일함

#### 2. PF Weight의 영향 확인
- PF weight가 매우 작은 이유 확인 (avg_rate가 높기 때문)
- PF weight의 영향을 줄이거나 Priority의 가중치를 높이는 방법 검토

#### 3. PID 파라미터 조정
- 현재 PID 파라미터: `kp=1.0, ki=0.1, kd=0.01`
- 더 공격적인 조정이 필요할 수 있음 (예: `ki` 값 증가)

#### 4. 제어 주기 조정
- 현재 제어 주기: 1초
- 더 짧은 주기로 변경하여 빠른 반응 가능 (단, 시스템 부하 고려)

### 검증 방법

**로그 확인 명령어**:
```bash
# 1. Throughput Controller Priority 적용 확인
grep "Throughput Controller Priority 적용" /tmp/gnb.log | tail -20

# 2. Priority 계산 확인
grep "PID control.*target_priority=" /tmp/gnb.log | tail -20

# 3. 스케줄러 Priority 적용 확인
grep "min_combined_prio=" /tmp/gnb.log | tail -20

# 4. 스루풋 변화 추이 확인
grep "DSCP adjusted.*throughput=" /tmp/gnb.log | tail -30

# 5. final_priority 확인
grep "DL Final Priority" /tmp/gnb.log | tail -20
```

**기대되는 결과**:
- Priority 변경 → `combined_prio` 변경 → `prio_weight` 변경 → `final_priority` 변경
- 시간 경과에 따라 스루풋이 목표값에 수렴

### 참고 사항

1. **Priority와 스루풋의 관계**:
   - Priority 값이 낮을수록 높은 우선순위 = 더 많은 리소스 할당 = 더 높은 스루풋
   - Priority 값이 높을수록 낮은 우선순위 = 더 적은 리소스 할당 = 더 낮은 스루풋

2. **제어 지연**:
   - Priority 변경 → 다음 스케줄링 슬롯 반영 → 리소스 할당 변경 → 스루풋 측정
   - 전체 과정에 시간이 소요되므로 즉각적인 변화는 기대하기 어려움

3. **PID 제어기 특성**:
   - 적분 항(I)이 누적되어 시간이 지나면서 오차가 보정됨
   - 충분한 시간을 주면 목표값에 수렴해야 함

