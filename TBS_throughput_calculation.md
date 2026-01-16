# TBS와 Throughput 계산 정리

## 개요

이 문서는 srsRAN에서 Transmission Block Size (TBS)와 Throughput 계산에 대한 내용을 정리합니다.

**코드 위치**: `lib/scheduler/logging/scheduler_metrics_handler.cpp`

---

## 1. TBS (Transmission Block Size)란?

### 정의

- **TBS** = **tb_size_bytes** = Transmission Block Size (bytes 단위)
- 스케줄러가 한 번에 전송하는 데이터 블록의 크기 (bytes)
- 한 번의 PDSCH/PUSCH 전송에서 전송되는 실제 데이터 크기

### 코드 위치

```cpp
// lib/scheduler/logging/scheduler_result_logger.cpp
tbs = ue_msg.pdsch_cfg.codewords[0].tb_size_bytes
```

### 로그 예시

```
DL: ue=0 c-rnti=0x4601 h_id=0 ... tbs=301
```

- `tbs=7` → 7 bytes 전송
- `tbs=301` → 301 bytes 전송
- `tbs=3841` → 3841 bytes (약 3.75 KB) 전송

### 특징

- **작은 tbs** (7, 11, 16 bytes): 제어/신호 데이터
- **큰 tbs** (3841 bytes): 사용자 데이터 (TCP 패킷 등)
- MCS (Modulation and Coding Scheme), RB (Resource Block) 수에 따라 결정됨

---

## 2. Throughput 계산에서 TBS의 중요성

### TBS는 Throughput 계산의 핵심

Throughput은 **성공한 TB의 크기(TBS)를 누적**하여 계산됩니다.

### 계산 과정

#### Step 1: HARQ ACK 수신 시 TBS 누적

**코드 위치**: `lib/scheduler/logging/scheduler_metrics_handler.cpp:241-251`

```cpp
void cell_metrics_handler::handle_dl_harq_ack(du_ue_index_t ue_index, bool ack, units::bytes tbs)
{
  if (ues.contains(ue_index)) {
    auto& u = ues[ue_index];
    u.data.count_uci_harq_acks += ack ? 1 : 0;
    ++u.data.count_uci_harqs;
    if (ack) {
      u.data.sum_dl_tb_bytes += tbs.value();  // ✅ 성공한 TB의 크기 누적
    }
  }
}
```

**중요 포인트:**
- HARQ ACK가 성공(`ack = true`)인 경우에만 TBS를 누적
- 실패한 전송은 throughput 계산에 포함되지 않음

#### Step 2: Throughput 계산

**코드 위치**: `lib/scheduler/logging/scheduler_metrics_handler.cpp:634-635`

```cpp
ret.dl_brate_kbps = static_cast<double>(data.sum_dl_tb_bytes * 8U) / metric_report_period.count();
ret.ul_brate_kbps = static_cast<double>(data.sum_ul_tb_bytes * 8U) / metric_report_period.count();
```

### Throughput 계산 공식

```
Throughput (kbps) = (누적된 성공한 TB bytes × 8) / 리포트 기간 (ms)
                  = (sum_dl_tb_bytes × 8 bits/byte) / metric_report_period.count() ms
                  = bits / ms
                  = kbits / s (kbps)
```

### 계산 예시

**조건:**
- `sum_dl_tb_bytes = 1,000,000 bytes` (성공한 전송 누적)
- `metric_report_period = 1000 ms` (1초)

**계산:**
```
dl_brate_kbps = (1,000,000 bytes × 8 bits/byte) / 1000 ms
              = 8,000,000 bits / 1000 ms
              = 8,000 kbits/s
              = 8,000 kbps
              = 8 Mbps
```

---

## 3. 단위 변환: bytes → bits

### bytes × 8 = bits

```cpp
data.sum_dl_tb_bytes * 8U
```

- `sum_dl_tb_bytes`: bytes 단위
- `* 8U`: bits로 변환 (1 byte = 8 bits)
- 결과: bits 단위

**이유**: Throughput은 일반적으로 bits per second (bps) 단위로 표현되기 때문

---

## 4. metric_report_period: 리포트 기간

### 정의

**코드 위치**: `lib/scheduler/logging/scheduler_metrics_handler.cpp:407, 616`

```cpp
// 리포트 기간 계산 (407줄)
const std::chrono::milliseconds report_period{
    data.nof_slots / last_slot_tx.nof_slots_per_subframe()
};

// 함수 파라미터 (616줄)
compute_report(std::chrono::milliseconds metric_report_period, ...)
```

### 단위

- **타입**: `std::chrono::milliseconds`
- **단위**: milliseconds (ms)
- **의미**: Metrics를 수집한 시간 구간 (리포트 기간)

### 계산 방법

```
report_period = nof_slots / nof_slots_per_subframe()
              = slots / (slots/subframe)
              = subframes
              = milliseconds (1 subframe = 1 ms)
```

### 로그 출력

**코드 위치**: `lib/scheduler/logging/scheduler_metrics_handler.cpp:639-641`

```cpp
logger.info("UE{} Throughput calc: sum_dl_tb_bytes={}, period={}ms, dl_brate_kbps={:.2f} ...",
            ue_index, data.sum_dl_tb_bytes, metric_report_period.count(), ...);
```

로그 예시:
```
UE0 Throughput calc: sum_dl_tb_bytes=1000000, period=1000ms, dl_brate_kbps=8000.00 ...
```

---

## 5. 로그의 tbs vs 실제 계산의 tbs

### 차이점

| 구분 | 로그의 tbs | 실제 계산의 tbs |
|------|-----------|----------------|
| **의미** | 스케줄링 시점에 예상된 TB 크기 | HARQ ACK를 받은 실제 전송된 TB 크기 |
| **출력 위치** | `scheduler_result_logger.cpp` | `scheduler_metrics_handler.cpp` |
| **용도** | 스케줄링 결정 로그 | Throughput 계산 |

### 로그의 tbs

```cpp
// lib/scheduler/logging/scheduler_result_logger.cpp
tbs = ue_msg.pdsch_cfg.codewords[0].tb_size_bytes
```

- 스케줄러가 전송을 결정할 때 예상되는 TB 크기
- 실제로 전송되었는지와 무관하게 스케줄링 결정에 사용된 값

### 실제 계산의 tbs

```cpp
// lib/scheduler/logging/scheduler_metrics_handler.cpp
void handle_dl_harq_ack(du_ue_index_t ue_index, bool ack, units::bytes tbs)
{
  if (ack) {
    u.data.sum_dl_tb_bytes += tbs.value();  // 실제 전송 성공한 TB 크기
  }
}
```

- HARQ ACK를 받은 실제 전송된 TB의 크기
- Throughput 계산에 사용되는 실제 값

---

## 6. 요약

### 핵심 포인트

1. **TBS는 Throughput 계산의 핵심**
   - Throughput = 누적된 성공한 TB bytes / 시간
   - TBS 없이는 throughput을 계산할 수 없음

2. **성공한 전송만 계산**
   - HARQ ACK가 성공(`ack = true`)인 경우에만 TBS 누적
   - 실패한 전송은 throughput 계산에 포함되지 않음

3. **단위 변환**
   - bytes × 8 = bits (Throughput은 bps 단위)

4. **리포트 기간**
   - `metric_report_period.count()` = milliseconds 단위
   - Metrics를 수집한 시간 구간

### Throughput 계산 전체 흐름

```
1. 스케줄러가 데이터 전송 (tbs 결정)
   ↓
2. HARQ ACK 수신 (성공/실패 확인)
   ↓
3. 성공한 경우만 TBS 누적 (sum_dl_tb_bytes += tbs)
   ↓
4. 리포트 기간마다 Throughput 계산
   ↓
   Throughput = (sum_dl_tb_bytes × 8) / metric_report_period.count()
```

---

## 7. GBR (Guaranteed Bit Rate) - 코어에서 받아오는 방법

### 정의

- **GBR (Guaranteed Bit Rate)**: 코어 네트워크에서 보장해야 하는 최소 비트레이트
- **gbr_dl**: Downlink GBR (bps 단위)
- **gbr_ul**: Uplink GBR (bps 단위)
- **max_br_dl/ul**: Maximum Bit Rate (bps 단위)

### 코어에서 GBR을 받아오는 경로

GBR 정보는 코어 네트워크(AMF)에서 시작하여 여러 인터페이스를 거쳐 DU의 스케줄러까지 전달됩니다.

```
AMF (Core Network)
  ↓ NGAP
  ↓ guaranteed_flow_bit_rate_dl/ul
CU-CP
  ↓ E1AP (CU-CP → CU-UP)
  ↓ guaranteed_flow_bit_rate_dl/ul
CU-UP
  ↓ F1AP (CU-CP → DU)
  ↓ guaranteed_flow_bit_rate_dl/ul
DU (스케줄러)
```

### 1. NGAP (AMF → CU-CP)

**파일**: `lib/ngap/ngap_asn1_converters.h`

**함수**: `ngap_asn1_to_gbr_qos_flow_information()` (864-871줄)

```cpp
inline gbr_qos_flow_information
ngap_asn1_to_gbr_qos_flow_information(const asn1::ngap::gbr_qos_info_s& asn1_gbr_qos_info)
{
  gbr_qos_flow_information gbr_qos_info;
  gbr_qos_info.max_br_dl = asn1_gbr_qos_info.max_flow_bit_rate_dl;
  gbr_qos_info.max_br_ul = asn1_gbr_qos_info.max_flow_bit_rate_ul;
  gbr_qos_info.gbr_dl    = asn1_gbr_qos_info.guaranteed_flow_bit_rate_dl;  // ✅ 코어에서 받아옴
  gbr_qos_info.gbr_ul    = asn1_gbr_qos_info.guaranteed_flow_bit_rate_ul;  // ✅ 코어에서 받아옴
  // ...
}
```

**경로**: AMF (Core Network) → NGAP → CU-CP

### 2. E1AP (CU-CP → CU-UP)

**파일**: `lib/e1ap/common/e1ap_asn1_converters.h`

**함수**: `asn1_e1ap_to_qos_flow_map_item()` 내부 (1168-1185줄)

```cpp
// Fill GBR QoS flow info.
if (asn1_flow_map_item.qos_flow_level_qos_params.gbr_qos_flow_info_present) {
  gbr_qos_flow_information gbr_qos_flow_info;
  gbr_qos_flow_info.max_br_dl = asn1_flow_map_item.qos_flow_level_qos_params.gbr_qos_flow_info.max_flow_bit_rate_dl;
  gbr_qos_flow_info.max_br_ul = asn1_flow_map_item.qos_flow_level_qos_params.gbr_qos_flow_info.max_flow_bit_rate_ul;
  gbr_qos_flow_info.gbr_dl =
      asn1_flow_map_item.qos_flow_level_qos_params.gbr_qos_flow_info.guaranteed_flow_bit_rate_dl;  // ✅ 전달
  gbr_qos_flow_info.gbr_ul =
      asn1_flow_map_item.qos_flow_level_qos_params.gbr_qos_flow_info.guaranteed_flow_bit_rate_ul;  // ✅ 전달
  // ...
}
```

**경로**: CU-CP → E1AP → CU-UP

### 3. F1AP (CU-CP → DU)

**파일**: `lib/f1ap/asn1_helpers.cpp`

**함수**: `drb_info_from_f1ap_asn1()` (242-247줄)

```cpp
// Note: As per TS 48.473, 9.3.1.45, "This IE shall be present for GBR QoS Flows only and is ignored otherwise."
if (is_gbr and asn1_drb_info.drb_qos.gbr_qos_flow_info_present) {
    auto& gbr     = out.drb_qos.gbr_qos_info.emplace();
    gbr.max_br_dl = asn1_drb_info.drb_qos.gbr_qos_flow_info.max_flow_bit_rate_dl;
    gbr.max_br_ul = asn1_drb_info.drb_qos.gbr_qos_flow_info.max_flow_bit_rate_ul;
    gbr.gbr_dl    = asn1_drb_info.drb_qos.gbr_qos_flow_info.guaranteed_flow_bit_rate_dl;  // ✅ DU로 전달
    gbr.gbr_ul    = asn1_drb_info.drb_qos.gbr_qos_flow_info.guaranteed_flow_bit_rate_ul;  // ✅ DU로 전달
    // ...
}
```

**경로**: CU-CP → F1AP → DU

### GBR의 용도

GBR은 스케줄러에서 **gbr_weight** 계산에 사용됩니다:

**코드 위치**: `lib/scheduler/policy/scheduler_time_qos.cpp`

```cpp
// GBR flow
if (lc->qos->runtime_gbr_qos_info.has_value()) {
  const unsigned dl_avg_rate = u.dl_logical_channels().avg_bytes_per_slot(lc->lcid) * 8U *
                                slot_tx.nof_slots_per_subframe();
  if (dl_avg_rate > 0) {
    gbr_weight += std::min(lc->qos->runtime_gbr_qos_info->gbr_dl / dl_avg_rate, max_metric_weight);
  } else {
    gbr_weight += max_metric_weight;  // 평균 비트레이트가 0이면 최대 가중치
  }
}
```

**계산 공식**:
```
gbr_weight = gbr_dl / dl_avg_rate
```

- `gbr_dl`: 코어에서 받은 GBR (bps)
- `dl_avg_rate`: 실제 평균 다운링크 비트레이트 (bps)
- `gbr_weight > 1.0`: GBR 목표를 달성하지 못함 → 우선순위 증가
- `gbr_weight = 1.0`: GBR 목표 달성 또는 Non-GBR 플로우

### GBR과 Throughput의 관계

- **GBR**: 코어에서 요구하는 **목표 비트레이트** (bps)
- **Throughput**: 실제로 달성한 **평균 비트레이트** (kbps)
- **gbr_weight**: 목표 대비 실제 성능 비율
  - `gbr_weight = gbr_dl / dl_avg_rate`
  - Throughput이 낮으면 `gbr_weight`가 증가하여 우선순위 상승

### 요약

1. **GBR은 코어 네트워크(AMF)에서 시작**
   - NGAP → E1AP → F1AP를 거쳐 DU까지 전달

2. **ASN.1 필드명**: `guaranteed_flow_bit_rate_dl/ul`
   - 모든 인터페이스에서 동일한 필드명 사용

3. **최종 저장 위치**: `gbr_qos_flow_information` 구조체
   - `gbr_dl`, `gbr_ul` 필드에 저장 (bps 단위)

4. **스케줄러에서 사용**
   - `gbr_weight` 계산에 사용
   - GBR 목표 달성 여부에 따라 우선순위 조정

---

## 참고 파일

- **Throughput 계산**: `lib/scheduler/logging/scheduler_metrics_handler.cpp`
  - HARQ ACK 처리: `handle_dl_harq_ack()` (241-251줄)
  - Throughput 계산: `compute_report()` (634-635줄)
  
- **TBS 로그 출력**: `lib/scheduler/logging/scheduler_result_logger.cpp`
  - DL 로그: `make_ue_dl_msg_log_entry()` (259줄)
  - UL 로그: `make_ue_ul_msg_log_entry()` (302줄)

- **GBR 수신**:
  - NGAP: `lib/ngap/ngap_asn1_converters.h` - `ngap_asn1_to_gbr_qos_flow_information()` (864-871줄)
  - E1AP: `lib/e1ap/common/e1ap_asn1_converters.h` - `asn1_e1ap_to_qos_flow_map_item()` (1168-1185줄)
  - F1AP: `lib/f1ap/asn1_helpers.cpp` - `drb_info_from_f1ap_asn1()` (242-247줄)

- **GBR 사용**: `lib/scheduler/policy/scheduler_time_qos.cpp`
  - `compute_dl_qos_weights()` - gbr_weight 계산

---

## 관련 문서

- [5QI 트래픽 요구 유형별 분류](./5QI_TRAFFIC_CLASSIFICATION.md)
- [DSCP 5QI 매핑 검증](./DSCP_5QI_MAPPING_VALIDATION.md)

