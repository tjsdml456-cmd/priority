# 5QI 트래픽 요구 타입 분류

## 개요
5QI (5G QoS Identifier)는 TS 23.501 표준에 따라 다양한 트래픽 요구사항에 맞춰 설계되었습니다. 
srsRAN 구현을 기반으로 트래픽 요구 타입을 분류합니다.

## QoS 특성 정의

5QI는 `standardized_qos_characteristics` 구조체에 정의된 여러 특성들의 조합으로 정의됩니다:

### 주요 특성 (모든 5QI에 공통):

- **Band (대역폭 보장)**: `res_type` (qos_flow_resource_type) - GBR 여부
  - `gbr`: 보장 비트레이트 있음
  - `non_gbr`: 보장 비트레이트 없음
  - `delay_critical_gbr`: 지연 민감 보장 비트레이트 있음
  
- **Delay (지연)**: `packet_delay_budget_ms` - 패킷 지연 예산 (단위: ms)

- **Reliability (신뢰성)**: `per` (packet_error_rate_t) - 패킷 오류율 (예: 10⁻⁶, 10⁻⁵, 10⁻⁴, 10⁻³, 10⁻²)

- **Priority (우선순위)**: `priority` (qos_prio_level_t) - 스케줄링 우선순위 (낮은 숫자가 높은 우선순위)

### 추가 특성 (조건부):

- **Average Window (평균 윈도우)**: `average_window_ms` (optional) - GBR 타입일 때만 사용 (기본값: 2000ms)

- **Maximum Data Burst Volume (최대 데이터 버스트 볼륨)**: `max_data_burst_volume` (optional) - Delay Critical GBR일 때만 사용

> **참고**: 코드에서 `asn1_helpers.cpp`는 `res_type == gbr || delay_critical_gbr`로 GBR 여부를 체크하고, `scheduler_time_qos.cpp`에서는 `priority`와 `packet_delay_budget_ms`를 사용하여 스케줄링 가중치를 계산합니다.

## QoS 특성별 스케줄러 동작 및 코드 위치

### 1. Band (GBR 여부) - `res_type` / `gbr_qos_info`

#### 스케줄러에서의 동작

**파일**: `lib/scheduler/policy/scheduler_time_qos.cpp:179-190` (DL), `scheduler_time_qos.cpp:236-248` (UL)

**동작 방식**:
```cpp
if (not lc->qos->gbr_qos_info.has_value()) {
  // LC is a non-GBR flow.
  continue;  // Non-GBR은 GBR 가중치 계산에서 제외
}

// GBR flow
double dl_avg_rate = u.dl_avg_bit_rate(lc->lcid);
if (dl_avg_rate != 0) {
  gbr_weight += std::min(lc->qos->gbr_qos_info->gbr_dl / dl_avg_rate, max_metric_weight);
} else {
  gbr_weight += max_metric_weight;  // 평균 비트레이트가 0이면 최대 가중치
}
```

- **GBR**: `gbr_weight = gbr_dl / dl_avg_rate` 계산 (목표 비트레이트/실제 평균 비트레이트). 비율이 1보다 크면 가중치가 증가하여 우선순위 상승.
- **Non-GBR**: GBR 가중치 계산에서 제외, `gbr_weight = 1.0` 설정

#### 코드 위치 (스케줄러 외부)

- **정의**: `include/srsran/ran/qos/five_qi_qos_mapping.h:34` - `enum class qos_flow_resource_type { gbr, non_gbr, delay_critical_gbr }`
- **매핑**: `lib/ran/qos/five_qi_qos_mapping.cpp:36-79` - 각 5QI의 `res_type` 값 정의
- **GBR 여부 체크**: `lib/f1ap/asn1_helpers.cpp:238-239` - `res_type == qos_flow_resource_type::gbr || delay_critical_gbr`로 GBR 체크
- **GBR 정보 변환**: 
  - `lib/f1ap/asn1_helpers.cpp:242-254` - F1AP ASN1에서 GBR QoS 정보 변환
  - `lib/ngap/ngap_asn1_converters.h:862-879` - NGAP ASN1에서 GBR QoS 정보 변환
  - `lib/e1ap/common/e1ap_asn1_converters.h:1172-1175` - E1AP ASN1에서 GBR QoS 정보 변환
- **MAC 설정**: `lib/mac/config/mac_config_helpers.cpp:107` - GBR UL을 PBR (Prioritized Bit Rate)로 변환
- **DRB 설정**: `lib/du/du_high/du_manager/ran_resource_management/du_bearer_resource_manager.cpp:207-209` - GBR DRB의 MAC LC 설정

---

### 2. Delay (PDB) - `packet_delay_budget_ms`

#### 스케줄러에서의 동작

**파일**: `lib/scheduler/policy/scheduler_time_qos.cpp:172-177` (DL만, UL은 사용 안 함)

**동작 방식**:
```cpp
slot_point hol_toa = u.dl_hol_toa(lc->lcid);  // Head of Line 도착 시간
if (hol_toa.valid() and slot_tx >= hol_toa) {
  const unsigned hol_delay_ms = (slot_tx - hol_toa) / slot_tx.nof_slots_per_subframe();
  const unsigned pdb = lc->qos->qos.packet_delay_budget_ms;
  delay_weight += hol_delay_ms / static_cast<double>(pdb);
}
```

- **`delay_weight = hol_delay_ms / pdb`**: 현재 HOL (Head of Line) 지연을 PDB로 나눈 비율
- 지연이 PDB에 가까울수록 가중치가 커져 우선순위 상승
- 예: PDB 100ms, 현재 지연 80ms → `delay_weight = 0.8`

#### 코드 위치 (스케줄러 외부)

- **정의**: `include/srsran/ran/qos/five_qi_qos_mapping.h:48` - `unsigned packet_delay_budget_ms`
- **매핑**: `lib/ran/qos/five_qi_qos_mapping.cpp:36-79` - 각 5QI의 PDB 값 정의
- **스케줄러에서만 사용**: PDB는 주로 스케줄러에서 지연 가중치 계산에만 사용됨

---

### 3. Reliability (PER) - `per`

#### 스케줄러에서의 동작

**직접 사용 없음**: PER은 스케줄러 가중치 계산에 직접 사용되지 않음.

PER은 다음에서 간접적으로 영향:
- 링크 어댑테이션 (MCS 선택)
- HARQ 재전송 정책
- 채널 코딩/변조 결정

#### 코드 위치 (스케줄러 외부)

- **정의**: `include/srsran/ran/qos/five_qi_qos_mapping.h:50` - `packet_error_rate_t per`
- **매핑**: `lib/ran/qos/five_qi_qos_mapping.cpp:36-79` - 각 5QI의 PER 값 정의 (예: `packet_error_rate_t::make(1e-6)`)
- **직접 사용**: 코드베이스에서 PER을 직접 사용하는 로직은 발견되지 않음 (표준에서 정의된 값이나 향후 사용 예정일 수 있음)

---

### 4. Priority - `priority` (qos_prio_level_t)

#### 스케줄러에서의 동작

**파일**: `lib/scheduler/policy/scheduler_time_qos.cpp:167-170, 200-202` (DL), `scheduler_time_qos.cpp:231-233, 255-257` (UL)

**동작 방식**:
```cpp
// 1. 모든 LC 중 가장 낮은 combined priority 찾기
min_combined_prio = std::min(
    static_cast<uint16_t>(lc->qos->qos.priority.value() * lc->qos->arp_priority.value()), 
    min_combined_prio);

// 2. priority weight 계산 (낮은 priority 값 = 높은 우선순위)
double prio_weight = policy_params.priority_enabled ? 
    (max_combined_prio_level + 1 - min_combined_prio) / static_cast<double>(max_combined_prio_level + 1)
    : 1.0;
```

- **`combined_priority = qos.priority * arp_priority`**: QoS 우선순위와 ARP 우선순위를 곱함
- **`prio_weight`**: 가장 낮은 combined priority를 기준으로 가중치 계산 (낮은 숫자 = 높은 우선순위)

**LC 우선순위 결정**: `lib/scheduler/ue_context/dl_logical_channel_manager.cpp:153-154`
```cpp
prio = cfg.qos.has_value() ? cfg.qos->qos.priority.value() * cfg.qos->arp_priority.value()
                           : qos_prio_level_t::max() * arp_prio_level_t::max();
```

#### 코드 위치 (스케줄러 외부)

- **정의**: `include/srsran/ran/qos/five_qi_qos_mapping.h:44` - `qos_prio_level_t priority`
- **매핑**: `lib/ran/qos/five_qi_qos_mapping.cpp:36-79` - 각 5QI의 priority 값 정의 (예: `qos_prio_level_t{20}`)
- **LC 우선순위**: `lib/scheduler/ue_context/dl_logical_channel_manager.cpp:153-154` - LC 할당 시 우선순위 결정
- **ASN1 변환**:
  - `lib/f1ap/asn1_helpers.cpp:269, 302-304` - F1AP ASN1 변환
  - `lib/e1ap/cu_cp/e1ap_cu_cp_asn1_helpers.h:49, 78-80` - E1AP CU-CP ASN1 변환
  - `lib/e1ap/cu_up/e1ap_cu_up_asn1_helpers.h:93-96` - E1AP CU-UP ASN1 변환
  - `lib/e1ap/common/e1ap_asn1_converters.h:1119, 1144-1145` - E1AP ASN1 변환

---

### 5. Average Window - `average_window_ms`

#### 스케줄러에서의 동작

**파일**: 
- `lib/scheduler/ue_context/dl_logical_channel_manager.cpp:183-184`
- `lib/scheduler/ue_context/ul_logical_channel_manager.cpp:111-112`

**동작 방식**:
```cpp
if (ch_cfg->qos.value().gbr_qos_info.has_value()) {
  unsigned win_size_msec = ch_cfg->qos.value().qos.average_window_ms.value();
  channels[ch_cfg->lcid].avg_bytes_per_slot.resize(win_size_msec * slots_per_sec / 1000);
}
```

- **용도**: GBR LC의 평균 비트레이트 계산 윈도우 크기 결정
- **기본값**: 2000ms (2초)
- 평균 비트레이트는 이 윈도우 크기로 슬라이딩 윈도우 방식으로 계산됨

#### 코드 위치 (스케줄러 외부)

- **정의**: `include/srsran/ran/qos/five_qi_qos_mapping.h:53` - `std::optional<unsigned> average_window_ms`
- **매핑**: `lib/ran/qos/five_qi_qos_mapping.cpp:36-79` - GBR 타입에만 2000ms로 설정, Non-GBR은 `std::nullopt`
- **사용**: 스케줄러의 LC 관리자에서만 사용됨

---

### 6. MDBV (Maximum Data Burst Volume) - `max_data_burst_volume`

#### 스케줄러에서의 동작

**사용 안 함**: 현재 구현에서 스케줄러가 MDBV를 직접 사용하는 부분 없음.

#### 코드 위치 (스케줄러 외부)

- **정의**: `include/srsran/ran/qos/five_qi_qos_mapping.h:57` - `std::optional<uint16_t> max_data_burst_volume`
- **매핑**: `lib/ran/qos/five_qi_qos_mapping.cpp:73-79` - Delay Critical GBR 타입에만 값 설정 (255 또는 1354)
- **사용**: 코드베이스에서 MDBV를 직접 사용하는 로직은 발견되지 않음 (표준에서 정의된 값이나 향후 사용 예정일 수 있음)

---

### 최종 스케줄링 우선순위 계산

**파일**: `lib/scheduler/policy/scheduler_time_qos.cpp:140`

```cpp
return gbr_weight * pf_weight * prio_weight * delay_weight;
```

**최종 우선순위 = GBR 가중치 × PF 가중치 × Priority 가중치 × Delay 가중치**

- **gbr_weight**: GBR 요구사항 충족도 (GBR일 때만 > 1.0)
- **pf_weight**: Proportional Fair 가중치 (공정성)
- **prio_weight**: Priority 기반 가중치
- **delay_weight**: Delay 요구사항 위반 정도 (DL만)

모든 가중치가 곱해져 최종 우선순위가 결정됩니다.

## 특성별 5QI 값 정렬

### 1. Band (대역폭 보장) - GBR 기준

**GBR 있음 (보장 비트레이트 있음)**:
- Delay Critical GBR: 5QI 82, 5QI 83, 5QI 84, 5QI 85
- GBR: 5QI 1, 5QI 2, 5QI 3, 5QI 4, 5QI 65, 5QI 66, 5QI 67

**GBR 없음 (보장 비트레이트 없음)**:
- Non-GBR: 5QI 5, 5QI 6, 5QI 7, 5QI 8, 5QI 9, 5QI 69, 5QI 70, 5QI 79, 5QI 80

### 2. Delay (지연) - PDB 기준

**PDB 값이 높은 것부터 낮은 것까지** (지연 허용이 큰 것부터 작은 것까지):

| PDB | 5QI 값 |
|-----|--------|
| 300ms | 5QI 4, 5QI 6, 5QI 8, 5QI 9 |
| 200ms | 5QI 70 |
| 150ms | 5QI 2 |
| 100ms | 5QI 1, 5QI 5, 5QI 7, 5QI 66, 5QI 67 |
| 75ms | 5QI 65 |
| 60ms | 5QI 69 |
| 50ms | 5QI 3, 5QI 79 |
| 30ms | 5QI 84 |
| 10ms | 5QI 80, 5QI 82, 5QI 83 |
| 5ms | 5QI 85 |

### 3. Reliability (신뢰성) - PER 기준

**PER 값이 높은 것부터 낮은 것까지** (오류율이 큰 것부터 작은 것까지):

| PER | 5QI 값 |
|-----|--------|
| 10⁻² | 5QI 1, 5QI 65, 5QI 66, 5QI 79 |
| 10⁻³ | 5QI 2, 5QI 3, 5QI 7, 5QI 67 |
| 10⁻⁴ | 5QI 82, 5QI 83 |
| 10⁻⁵ | 5QI 84, 5QI 85 |
| 10⁻⁶ | 5QI 4, 5QI 5, 5QI 6, 5QI 8, 5QI 9, 5QI 69, 5QI 70, 5QI 80 |

## DSCP (PHB) → 5QI 매핑

Nokia White Paper (Sept 2020)에 따른 DSCP와 5QI 매핑:

| 5QI | Resource Type | Priority | PDB | PER | DSCP (PHB) | DSCP 값 |
|-----|--------------|---------|-----|-----|-----------|---------|
| 1 | GBR | 20 | 100ms | 10⁻² | EF (Expedited Forwarding) | 44 |
| 2 | GBR | 40 | 150ms | 10⁻³ | AF41 (Assured Forwarding) | 34 |
| 3 | GBR | 30 | 50ms | 10⁻³ | CS4 (Class Selector) | 32 |
| 4 | GBR | 50 | 300ms | 10⁻⁶ | AF32 (Assured Forwarding) | 28 |
| 5 | Non-GBR | 10 | 100ms | 10⁻⁶ | CS5 (Class Selector) | 40 |
| 6 | Non-GBR | 60 | 300ms | 10⁻⁶ | AF31 (Assured Forwarding) | 26 |
| 7 | Non-GBR | 70 | 100ms | 10⁻³ | AF23 (Assured Forwarding) | 22 |
| 9 | Non-GBR | 90 | 300ms | 10⁻⁶ | CS0 (Default Forwarding) | 0 |
| 66 | GBR | 20 | 100ms | 10⁻² | EF (Expedited Forwarding) | 44 |
| 67 | GBR | 15 | 100ms | 10⁻³ | AF43 (Assured Forwarding) | 38 |
| 70 | Non-GBR | 55 | 200ms | 10⁻⁶ | AF33 (Assured Forwarding) | 30 |
| 75 | GBR | 25 | 50ms | 10⁻² | CS4 (Class Selector) | 32 |
| 79 | Non-GBR | 65 | 50ms | 10⁻² | CS4 (Class Selector) | 32 |
| 80 | Non-GBR | 68 | 10ms | 10⁻⁶ | CS3 (Class Selector) | 24 |

> **참고**: 이 매핑은 Nokia White Paper (Sept 2020)의 권장사항이며, RFC4594 서비스 클래스 정의와 정렬되어 있습니다. 실제 네트워크 운영자의 정책에 따라 달라질 수 있습니다.

### 지연(PDB) 높은 순 → 낮은 순 정렬

**PDB 높은 것부터 낮은 것까지:**

| 5QI | Resource Type | Priority | PDB | PER | DSCP (PHB) | DSCP 값 |
|-----|--------------|---------|-----|-----|-----------|---------|
| 4 | GBR | 50 | 300ms | 10⁻⁶ | AF32 (Assured Forwarding) | 28 |
| 6 | Non-GBR | 60 | 300ms | 10⁻⁶ | AF31 (Assured Forwarding) | 26 |
| 9 | Non-GBR | 90 | 300ms | 10⁻⁶ | CS0 (Default Forwarding) | 0 |
| 70 | Non-GBR | 55 | 200ms | 10⁻⁶ | AF33 (Assured Forwarding) | 30 |
| 2 | GBR | 40 | 150ms | 10⁻³ | AF41 (Assured Forwarding) | 34 |
| 1 | GBR | 20 | 100ms | 10⁻² | EF (Expedited Forwarding) | 44 |
| 66 | GBR | 20 | 100ms | 10⁻² | EF (Expedited Forwarding) | 44 |
| 67 | GBR | 15 | 100ms | 10⁻³ | AF43 (Assured Forwarding) | 38 |
| 7 | Non-GBR | 70 | 100ms | 10⁻³ | AF23 (Assured Forwarding) | 22 |
| 5 | Non-GBR | 10 | 100ms | 10⁻⁶ | CS5 (Class Selector) | 40 |
| 79 | Non-GBR | 65 | 50ms | 10⁻² | CS4 (Class Selector) | 32 |
| 3 | GBR | 30 | 50ms | 10⁻³ | CS4 (Class Selector) | 32 |
| 80 | Non-GBR | 68 | 10ms | 10⁻⁶ | CS3 (Class Selector) | 24 |

### 신뢰성(PER) 높은 순 → 낮은 순 정렬

**PER 높은 것부터 낮은 것까지:**

| 5QI | Resource Type | Priority | PDB | PER | DSCP (PHB) | DSCP 값 |
|-----|--------------|---------|-----|-----|-----------|---------|
| 1 | GBR | 20 | 100ms | 10⁻² | EF (Expedited Forwarding) | 44 |
| 66 | GBR | 20 | 100ms | 10⁻² | EF (Expedited Forwarding) | 44 |
| 79 | Non-GBR | 65 | 50ms | 10⁻² | CS4 (Class Selector) | 32 |
| 2 | GBR | 40 | 150ms | 10⁻³ | AF41 (Assured Forwarding) | 34 |
| 3 | GBR | 30 | 50ms | 10⁻³ | CS4 (Class Selector) | 32 |
| 67 | GBR | 15 | 100ms | 10⁻³ | AF43 (Assured Forwarding) | 38 |
| 7 | Non-GBR | 70 | 100ms | 10⁻³ | AF23 (Assured Forwarding) | 22 |
| 4 | GBR | 50 | 300ms | 10⁻⁶ | AF32 (Assured Forwarding) | 28 |
| 5 | Non-GBR | 10 | 100ms | 10⁻⁶ | CS5 (Class Selector) | 40 |
| 6 | Non-GBR | 60 | 300ms | 10⁻⁶ | AF31 (Assured Forwarding) | 26 |
| 9 | Non-GBR | 90 | 300ms | 10⁻⁶ | CS0 (Default Forwarding) | 0 |
| 70 | Non-GBR | 55 | 200ms | 10⁻⁶ | AF33 (Assured Forwarding) | 30 |
| 80 | Non-GBR | 68 | 10ms | 10⁻⁶ | CS3 (Class Selector) | 24 |


## 1. 리소스 타입 (Resource Type) 기준 분류

### 1.1 GBR (Guaranteed Bit Rate) - 보장 비트레이트
**특징**: 최소 비트레이트가 보장되는 트래픽
- **평균 윈도우 (Averaging Window)**: 2000ms

**5QI 값**:
- **5QI 1**: 우선순위 20, PDB 100ms, PER 10⁻²
- **5QI 2**: 우선순위 40, PDB 150ms, PER 10⁻³
- **5QI 3**: 우선순위 30, PDB 50ms, PER 10⁻³
- **5QI 4**: 우선순위 50, PDB 300ms, PER 10⁻⁶
- **5QI 65**: 우선순위 7, PDB 75ms, PER 10⁻²
- **5QI 66**: 우선순위 20, PDB 100ms, PER 10⁻²
- **5QI 67**: 우선순위 15, PDB 100ms, PER 10⁻³

### 1.2 Non-GBR - 비보장 비트레이트
**특징**: 최소 비트레이트 보장 없음, 베스트 에포트 서비스
- **평균 윈도우**: 없음

**5QI 값**:
- **5QI 5**: 우선순위 10, PDB 100ms, PER 10⁻⁶
- **5QI 6**: 우선순위 60, PDB 300ms, PER 10⁻⁶
- **5QI 7**: 우선순위 70, PDB 100ms, PER 10⁻³
- **5QI 8**: 우선순위 80, PDB 300ms, PER 10⁻⁶
- **5QI 9**: 우선순위 90, PDB 300ms, PER 10⁻⁶
- **5QI 69**: 우선순위 5, PDB 60ms, PER 10⁻⁶
- **5QI 70**: 우선순위 55, PDB 200ms, PER 10⁻⁶
- **5QI 79**: 우선순위 65, PDB 50ms, PER 10⁻²
- **5QI 80**: 우선순위 68, PDB 10ms, PER 10⁻⁶

### 1.3 Delay Critical GBR - 지연 민감 보장 비트레이트
**특징**: 극도로 낮은 지연이 필요한 GBR 트래픽
- **평균 윈도우**: 2000ms
- **최대 데이터 버스트 볼륨 (MDBV)**: 정의됨

**5QI 값**:
- **5QI 82**: 우선순위 19, PDB 10ms, PER 10⁻⁴, MDBV 255
- **5QI 83**: 우선순위 22, PDB 10ms, PER 10⁻⁴, MDBV 1354
- **5QI 84**: 우선순위 24, PDB 30ms, PER 10⁻⁵, MDBV 1354
- **5QI 85**: 우선순위 21, PDB 5ms, PER 10⁻⁵, MDBV 255


## 2. 요약

5QI는 다음과 같은 **트래픽 요구 타입**에 맞춰 설계되었습니다:

1. **리소스 보장 요구**: GBR vs Non-GBR vs Delay Critical GBR
2. **지연 민감도**: 초저지연 (5-10ms) ~ 높은 지연 허용 (300ms+)
3. **신뢰성 요구**: 초고신뢰성 (10⁻⁶) ~ 낮은 신뢰성 허용 (10⁻²)
4. **우선순위 요구**: 최고 우선순위 (5) ~ 낮은 우선순위 (90)

각 5QI 값은 이러한 요구사항들의 조합으로 정의되어 있으며, 
네트워크가 다양한 애플리케이션의 QoS 요구사항을 효율적으로 처리할 수 있도록 합니다.

---

# 코드에서의 분류 증거

이 섹션은 위에서 분류한 내용이 실제 코드에서 어떻게 구분되어 있는지 증거를 제시합니다.

## 1. 리소스 타입 (Resource Type) 구분 증거

### 1.1 Enum 정의로 타입 구분

**파일**: `include/srsran/ran/qos/five_qi_qos_mapping.h`

```cpp
/// \brief Resource Type determines whether QoS Flow is of type Guaranteed Bit Rate (GBR), 
/// Delay-critical GBR or non-GBR. See TS 23.501, clause 5.7.3.2 Resource Type.
enum class qos_flow_resource_type { gbr, non_gbr, delay_critical_gbr };
```

**증거**: 코드에서 3가지 리소스 타입을 enum으로 명시적으로 구분하고 있습니다.

### 1.2 매핑 맵에서 타입별 그룹화

**파일**: `lib/ran/qos/five_qi_qos_mapping.cpp`

```cpp
static const std::unordered_map<five_qi_t, qos_chars> five_qi_to_qos_mapping = {
    // GBR.
    {uint_to_five_qi(1),
     qos_chars{flow_type::gbr, qos_prio_level_t{20}, 100, packet_error_rate_t::make(1e-2), 2000, std::nullopt}},
    {uint_to_five_qi(2),
     qos_chars{flow_type::gbr, qos_prio_level_t{40}, 150, packet_error_rate_t::make(1e-3), 2000, std::nullopt}},
    // ... (5QI 3, 4, 65, 66, 67도 GBR)

    // Non-GBR.
    {uint_to_five_qi(5),
     qos_chars{flow_type::non_gbr, qos_prio_level_t{10}, 100, packet_error_rate_t::make(1e-6), std::nullopt, std::nullopt}},
    {uint_to_five_qi(6),
     qos_chars{flow_type::non_gbr, qos_prio_level_t{60}, 300, packet_error_rate_t::make(1e-6), std::nullopt, std::nullopt}},
    // ... (5QI 7, 8, 9, 69, 70, 79, 80도 Non-GBR)

    // Delay Critical GBR.
    {uint_to_five_qi(82),
     qos_chars{flow_type::delay_critical_gbr, qos_prio_level_t{19}, 10, packet_error_rate_t::make(1e-4), 2000, 255}},
    {uint_to_five_qi(83),
     qos_chars{flow_type::delay_critical_gbr, qos_prio_level_t{22}, 10, packet_error_rate_t::make(1e-4), 2000, 1354}},
    // ... (5QI 84, 85도 Delay Critical GBR)
};
```

**증거**: 
- 주석으로 `// GBR.`, `// Non-GBR.`, `// Delay Critical GBR.` 명시
- 각 5QI 값이 `flow_type::gbr`, `flow_type::non_gbr`, `flow_type::delay_critical_gbr`로 명시적으로 지정됨

### 1.3 실제 사용처에서 타입 체크

**파일**: `lib/f1ap/asn1_helpers.cpp` (238-239줄)

```cpp
const auto* five_qi_params = get_5qi_to_qos_characteristics_mapping(nondyn_5qi.five_qi);
const bool  is_gbr         = five_qi_params && (five_qi_params->res_type == qos_flow_resource_type::gbr ||
                                       five_qi_params->res_type == qos_flow_resource_type::delay_critical_gbr);
```

**증거**: 코드에서 GBR 타입을 체크할 때 `res_type` 필드를 사용하여 `gbr` 또는 `delay_critical_gbr`인지 확인합니다.

## 2. 구조체 필드로 특성값 저장

**파일**: `include/srsran/ran/qos/five_qi_qos_mapping.h` (40-57줄)

```cpp
struct standardized_qos_characteristics {
  qos_flow_resource_type res_type;              // 리소스 타입 (GBR/Non-GBR/Delay Critical GBR)
  qos_prio_level_t priority;                    // 우선순위 레벨
  unsigned packet_delay_budget_ms;              // 패킷 지연 예산 (PDB)
  packet_error_rate_t per;                      // 패킷 오류율 (PER)
  std::optional<unsigned> average_window_ms;    // 평균 윈도우 (GBR만)
  std::optional<uint16_t> max_data_burst_volume; // 최대 데이터 버스트 볼륨 (Delay Critical GBR만)
};
```

**증거**: 
- 각 특성값이 구조체의 필드로 정의되어 있음
- `average_window_ms`는 GBR 타입에만 사용 (Non-GBR은 `std::nullopt`)
- `max_data_burst_volume`는 Delay Critical GBR에만 사용

### 2.1 GBR vs Non-GBR 구분 증거

**코드에서의 차이점**:

1. **GBR** (5QI 1, 2, 3, 4, 65, 66, 67):
   ```cpp
   qos_chars{flow_type::gbr, ..., 2000, std::nullopt}
   // average_window_ms = 2000 (값 존재)
   // max_data_burst_volume = std::nullopt (값 없음)
   ```

2. **Non-GBR** (5QI 5, 6, 7, 8, 9, 69, 70, 79, 80):
   ```cpp
   qos_chars{flow_type::non_gbr, ..., std::nullopt, std::nullopt}
   // average_window_ms = std::nullopt (값 없음)
   // max_data_burst_volume = std::nullopt (값 없음)
   ```

3. **Delay Critical GBR** (5QI 82, 83, 84, 85):
   ```cpp
   qos_chars{flow_type::delay_critical_gbr, ..., 2000, 255}
   // average_window_ms = 2000 (값 존재)
   // max_data_burst_volume = 255 또는 1354 (값 존재)
   ```

## 3. 우선순위 (Priority) 구분 증거

**파일**: `lib/ran/qos/five_qi_qos_mapping.cpp`

각 5QI 값의 우선순위가 명시적으로 지정되어 있습니다:

```cpp
// 우선순위 5-10 (최고 우선순위)
{uint_to_five_qi(69), qos_chars{..., qos_prio_level_t{5}, ...}}   // Priority 5
{uint_to_five_qi(5),  qos_chars{..., qos_prio_level_t{10}, ...}}  // Priority 10

// 우선순위 15-30 (높은 우선순위)
{uint_to_five_qi(67), qos_chars{..., qos_prio_level_t{15}, ...}}  // Priority 15
{uint_to_five_qi(1),  qos_chars{..., qos_prio_level_t{20}, ...}}  // Priority 20
{uint_to_five_qi(3),  qos_chars{..., qos_prio_level_t{30}, ...}}  // Priority 30

// 우선순위 40-70 (중간 우선순위)
{uint_to_five_qi(2),  qos_chars{..., qos_prio_level_t{40}, ...}}  // Priority 40
{uint_to_five_qi(4),  qos_chars{..., qos_prio_level_t{50}, ...}}  // Priority 50
{uint_to_five_qi(70), qos_chars{..., qos_prio_level_t{55}, ...}}  // Priority 55
{uint_to_five_qi(6),  qos_chars{..., qos_prio_level_t{60}, ...}}  // Priority 60
{uint_to_five_qi(79), qos_chars{..., qos_prio_level_t{65}, ...}}  // Priority 65
{uint_to_five_qi(80), qos_chars{..., qos_prio_level_t{68}, ...}}  // Priority 68
{uint_to_five_qi(7),  qos_chars{..., qos_prio_level_t{70}, ...}}  // Priority 70

// 우선순위 80-90 (낮은 우선순위)
{uint_to_five_qi(8),  qos_chars{..., qos_prio_level_t{80}, ...}}  // Priority 80
{uint_to_five_qi(9),  qos_chars{..., qos_prio_level_t{90}, ...}}  // Priority 90
```

**증거**: 각 5QI 값의 우선순위가 `qos_prio_level_t{값}` 형태로 명시적으로 지정되어 있습니다.

## 4. 지연 예산 (PDB) 구분 증거

**파일**: `lib/ran/qos/five_qi_qos_mapping.cpp`

각 5QI 값의 PDB가 숫자로 명시되어 있습니다:

```cpp
// 초저지연 (PDB ≤ 10ms)
{uint_to_five_qi(80), qos_chars{..., 10, ...}}   // PDB 10ms
{uint_to_five_qi(82), qos_chars{..., 10, ...}}   // PDB 10ms
{uint_to_five_qi(83), qos_chars{..., 10, ...}}   // PDB 10ms
{uint_to_five_qi(85), qos_chars{..., 5, ...}}   // PDB 5ms

// 저지연 (PDB: 30-100ms)
{uint_to_five_qi(3),  qos_chars{..., 50, ...}}   // PDB 50ms
{uint_to_five_qi(5),  qos_chars{..., 100, ...}} // PDB 100ms
{uint_to_five_qi(1),  qos_chars{..., 100, ...}}  // PDB 100ms

// 중간 지연 (PDB: 150-200ms)
{uint_to_five_qi(2),  qos_chars{..., 150, ...}}  // PDB 150ms
{uint_to_five_qi(70), qos_chars{..., 200, ...}} // PDB 200ms

// 높은 지연 허용 (PDB ≥ 300ms)
{uint_to_five_qi(4),  qos_chars{..., 300, ...}}  // PDB 300ms
{uint_to_five_qi(6),  qos_chars{..., 300, ...}}  // PDB 300ms
{uint_to_five_qi(8),  qos_chars{..., 300, ...}}  // PDB 300ms
{uint_to_five_qi(9),  qos_chars{..., 300, ...}}  // PDB 300ms
```

**증거**: `packet_delay_budget_ms` 필드에 숫자 값으로 명시되어 있습니다.

## 5. 패킷 오류율 (PER) 구분 증거

**파일**: `lib/ran/qos/five_qi_qos_mapping.cpp`

각 5QI 값의 PER이 `packet_error_rate_t::make()` 함수로 명시되어 있습니다:

```cpp
// 초고신뢰성 (PER ≤ 10⁻⁶)
{uint_to_five_qi(4),  qos_chars{..., packet_error_rate_t::make(1e-6), ...}}
{uint_to_five_qi(5),  qos_chars{..., packet_error_rate_t::make(1e-6), ...}}
{uint_to_five_qi(6),  qos_chars{..., packet_error_rate_t::make(1e-6), ...}}
{uint_to_five_qi(8),  qos_chars{..., packet_error_rate_t::make(1e-6), ...}}
{uint_to_five_qi(9),  qos_chars{..., packet_error_rate_t::make(1e-6), ...}}
{uint_to_five_qi(80), qos_chars{..., packet_error_rate_t::make(1e-6), ...}}

// 고신뢰성 (PER: 10⁻⁵)
{uint_to_five_qi(84), qos_chars{..., packet_error_rate_t::make(1e-5), ...}}
{uint_to_five_qi(85), qos_chars{..., packet_error_rate_t::make(1e-5), ...}}

// 중간 신뢰성 (PER: 10⁻⁴)
{uint_to_five_qi(82), qos_chars{..., packet_error_rate_t::make(1e-4), ...}}
{uint_to_five_qi(83), qos_chars{..., packet_error_rate_t::make(1e-4), ...}}

// 낮은 신뢰성 허용 (PER ≥ 10⁻³)
{uint_to_five_qi(1),  qos_chars{..., packet_error_rate_t::make(1e-2), ...}}
{uint_to_five_qi(2),  qos_chars{..., packet_error_rate_t::make(1e-3), ...}}
{uint_to_five_qi(3),  qos_chars{..., packet_error_rate_t::make(1e-3), ...}}
{uint_to_five_qi(7),  qos_chars{..., packet_error_rate_t::make(1e-3), ...}}
```

**증거**: `per` 필드에 `packet_error_rate_t::make(값)` 형태로 명시되어 있습니다.

## 6. 스케줄러에서의 실제 사용 증거

**파일**: `lib/scheduler/policy/scheduler_time_qos.cpp` (179-190줄)

```cpp
if (not lc->qos->gbr_qos_info.has_value()) {
  // LC is a non-GBR flow.
  continue;
}

// GBR flow.
double dl_avg_rate = u.dl_avg_bit_rate(lc->lcid);
if (dl_avg_rate != 0) {
  gbr_weight += std::min(lc->qos->gbr_qos_info->gbr_dl / dl_avg_rate, max_metric_weight);
} else {
  gbr_weight += max_metric_weight;
}
```

**증거**: 스케줄러에서 GBR과 Non-GBR을 다르게 처리합니다:
- Non-GBR: `gbr_qos_info.has_value()`가 false이면 스킵
- GBR: `gbr_qos_info`가 있으면 GBR 가중치 계산

**파일**: `lib/scheduler/policy/scheduler_time_qos.cpp` (175-176줄)

```cpp
const unsigned hol_delay_ms = (slot_tx - hol_toa) / slot_tx.nof_slots_per_subframe();
const unsigned pdb          = lc->qos->qos.packet_delay_budget_ms;
delay_weight += hol_delay_ms / static_cast<double>(pdb);
```

**증거**: PDB 값을 사용하여 지연 가중치를 계산합니다.

## 7. 요약: 코드에서의 분류 증거

| 분류 기준 | 코드 증거 위치 | 증거 내용 |
|---------|-------------|----------|
| **리소스 타입** | `five_qi_qos_mapping.h:34` | `enum class qos_flow_resource_type { gbr, non_gbr, delay_critical_gbr }` |
| **리소스 타입 매핑** | `five_qi_qos_mapping.cpp:36-79` | 각 5QI가 `flow_type::gbr/non_gbr/delay_critical_gbr`로 지정됨 |
| **타입 체크** | `asn1_helpers.cpp:238-239` | `res_type == qos_flow_resource_type::gbr` 체크 |
| **우선순위** | `five_qi_qos_mapping.cpp` | 각 5QI의 `qos_prio_level_t{값}` 명시 |
| **지연 예산** | `five_qi_qos_mapping.cpp` | 각 5QI의 `packet_delay_budget_ms` 숫자 값 |
| **오류율** | `five_qi_qos_mapping.cpp` | 각 5QI의 `packet_error_rate_t::make(값)` |
| **GBR 구분** | `scheduler_time_qos.cpp:179-190` | `gbr_qos_info.has_value()`로 GBR/Non-GBR 구분 |
| **지연 처리** | `scheduler_time_qos.cpp:175-176` | `packet_delay_budget_ms` 사용 |

**결론**: 위에서 분류한 모든 내용이 실제 코드에서 명시적으로 구분되어 있으며, enum, 구조체 필드, 매핑 맵, 실제 사용 로직 등에서 증거를 찾을 수 있습니다.
