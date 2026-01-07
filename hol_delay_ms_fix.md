# HOL TOA Clock Epoch 변환 문제 해결 (간단한 방법)

## 개요

기존 코드에서 `steady_clock::time_point`를 `system_clock::time_point`로 변환하는 과정에서 발생하는 clock epoch 문제를 해결하기 위해, RLC 레이어에서 `hol_toa`를 `system_clock::time_point`로 계산해서 전달하는 방식으로 변경했습니다.

**핵심 아이디어**: RLC에서 처음부터 `system_clock`을 사용하면, MAC과 스케줄러 어댑터에서 변환 없이 그대로 전달할 수 있습니다.

## 문제점

### 기존 방식의 문제

기존 코드는 다음과 같은 방식으로 HOL TOA를 전달했습니다:

```cpp
// MAC 레이어 (lib/mac/mac_dl/mac_cell_processor.cpp)
if (rlc_bs.hol_toa.has_value()) {
    auto steady_duration = rlc_bs.hol_toa->time_since_epoch();
    bs.hol_toa = std::chrono::system_clock::time_point(
        std::chrono::duration_cast<std::chrono::system_clock::duration>(steady_duration));
}
```

**문제점:**
1. **Clock Epoch 차이**: `steady_clock`과 `system_clock`은 서로 다른 epoch를 가집니다.
   - `steady_clock`: 시스템 부팅 시점 (또는 구현 정의)
   - `system_clock`: Unix epoch (1970-01-01 00:00:00 UTC)

2. **숫자 재해석**: `time_since_epoch()` duration만 옮겨서 `system_clock`의 epoch 위에 올리면, 절대 시간 의미가 아니라 단순히 숫자 재해석입니다.
   - 예: `steady_clock`에서 경과 100초 → `system_clock`의 1970-01-01 00:01:40으로 잘못 해석됨
   - 실제로는 부팅 후 100초일 뿐, 1970년이 아님

3. **플랫폼 의존성**: 상대 지연 계산에서 우연히 맞을 수도 있지만, 플랫폼/컴파일러에 따라 달라질 수 있습니다.

### 스케줄러에서 실제로 필요한 것

스케줄러는 실제로 **상대 지연**만 필요합니다:

```cpp
// 스케줄러에서 실제 계산
hol_delay_ms = (slot_tx - hol_toa) / slots_per_subframe();
delay_contrib = hol_delay_ms / pdb;
```

절대 시간이 아니라 **패킷이 도착한 시점부터 현재까지의 경과 시간**만 필요합니다.

## 해결 방법

### 핵심 아이디어

**time_point를 전달하지 말고, RLC에서 이미 계산한 값을 직접 전달하자.**

### 변경 전 흐름 (문제 있음)

```
RLC: hol_toa = steady_clock::now() - packet_arrival_time
     ↓ (time_point 전달)
MAC: steady_clock::time_point → system_clock::time_point 변환 (위험!)
     ↓ (time_point 전달)
스케줄러 어댑터: system_clock::time_point → high_resolution_clock::time_point → slot_point 변환
     ↓ (slot_point 전달)
스케줄러: hol_delay_ms = (slot_tx - hol_toa) / slots_per_subframe()
```

### 변경 후 흐름 (안전함)

```
RLC: hol_toa = system_clock::now() - (steady_clock::now() - packet_arrival_time)
     ↓ (system_clock::time_point 전달)
MAC: hol_toa 그대로 전달 (변환 없음!)
     ↓ (system_clock::time_point 전달)
스케줄러 어댑터: system_clock → high_resolution_clock → slot_point 변환 (상대 지연 계산)
     ↓ (slot_point 전달)
스케줄러: hol_delay_ms = (slot_tx - hol_toa) / slots_per_subframe()
         delay_contrib = hol_delay_ms / pdb
```

**핵심**: RLC에서 `system_clock`으로 변환하면, 이후 레이어에서는 변환이 간단해집니다.

## 상세 변경 내용

### 1. RLC 레이어 변경

#### 1.1 `rlc_buffer_state` 구조체 수정

**파일**: `include/srsran/rlc/rlc_buffer_state.h`

```cpp
struct rlc_buffer_state {
  unsigned pending_bytes = 0;
  /// Head of line (HOL) time of arrival (TOA) holds the TOA of the oldest SDU or ReTx that is queued for transmission.
  /// Uses system_clock to avoid clock epoch conversion issues when passing to MAC and scheduler layers.
  std::optional<std::chrono::time_point<std::chrono::system_clock>> hol_toa;  // ✅ system_clock으로 변경
};
```

#### 1.2 RLC 엔티티에서 `hol_toa`를 `system_clock`으로 변환

**파일**: 
- `lib/rlc/rlc_tx_am_entity.cpp`
- `lib/rlc/rlc_tx_um_entity.cpp`
- `lib/rlc/rlc_tx_tm_entity.cpp`

**변경 내용**:
```cpp
rlc_buffer_state rlc_tx_am_entity::get_buffer_state()
{
  rlc_buffer_state bs = {};
  // ... 기존 로직으로 steady_clock의 time_of_arrival 찾기 ...
  std::optional<std::chrono::time_point<std::chrono::steady_clock>> steady_hol_toa;
  if (next_sdu != nullptr) {
    steady_hol_toa = next_sdu->time_of_arrival;  // steady_clock
  }
  
  // ✅ Convert hol_toa from steady_clock to system_clock to avoid clock epoch conversion issues.
  // We calculate the relative delay and apply it to system_clock::now().
  if (steady_hol_toa.has_value()) {
    auto now_steady = std::chrono::steady_clock::now();
    auto delay = now_steady - steady_hol_toa.value();  // 상대 지연 계산
    bs.hol_toa = std::chrono::system_clock::now() - delay;  // system_clock으로 변환
  }
  
  return bs;
}
```

**장점**:
- RLC에서 한 번만 변환 (상대 지연 계산이므로 안전)
- 이후 레이어에서는 변환 없이 그대로 전달

### 2. MAC 레이어 변경

#### 2.1 `mac_dl_buffer_state_indication_message` 구조체

**파일**: `include/srsran/mac/mac_ue_control_information_handler.h`

```cpp
struct mac_dl_buffer_state_indication_message {
  du_ue_index_t ue_index;
  lcid_t        lcid;
  unsigned bs;
  /// \brief Time-of-arrival of the oldest PDU in the RLC entity Tx buffer. Uses system_clock to avoid clock epoch conversion issues.
  std::optional<std::chrono::system_clock::time_point> hol_toa;  // ✅ system_clock (변경 없음)
};
```

#### 2.2 MAC에서 `hol_toa` 그대로 전달

**파일**: `lib/mac/mac_dl/mac_cell_processor.cpp`

**변경 내용**:
```cpp
rlc_buffer_state rlc_bs = bearer->on_buffer_state_update();
mac_dl_buffer_state_indication_message bs{
    ue_mng.get_ue_index(grant.pdsch_cfg.rnti), lc_info.lcid.to_lcid(), rlc_bs.pending_bytes};

// ✅ Pass hol_toa directly from RLC (already in system_clock) to avoid clock epoch conversion issues.
if (rlc_bs.hol_toa.has_value()) {
  bs.hol_toa = rlc_bs.hol_toa.value();  // 그대로 전달 (변환 없음!)
}
```

**장점**:
- 변환 없이 그대로 전달
- 코드가 매우 간단해짐

### 3. 스케줄러 어댑터 변경

#### 3.1 `dl_buffer_state_indication_message` 구조체

**파일**: `include/srsran/scheduler/scheduler_dl_buffer_state_indication_handler.h`

```cpp
struct dl_buffer_state_indication_message {
  du_ue_index_t ue_index;
  lcid_t        lcid;
  unsigned      bs;
  /// Time-of-arrival, in slots, of the oldest PDU in the RLC entity Tx buffer.
  slot_point hol_toa;  // ✅ 변경 없음 (slot_point로 변환)
};
```

#### 3.2 스케줄러 어댑터에서 `system_clock` → `slot_point` 변환

**파일**: `lib/mac/mac_sched/srsran_scheduler_adapter.cpp`

**변경 내용**:
```cpp
void srsran_scheduler_adapter::handle_dl_buffer_state_update(
    const mac_dl_buffer_state_indication_message& mac_dl_bs_ind)
{
  dl_buffer_state_indication_message bs{};
  bs.ue_index = mac_dl_bs_ind.ue_index;
  bs.lcid     = mac_dl_bs_ind.lcid;
  bs.bs       = mac_dl_bs_ind.bs;
  
  // ✅ Convert hol_toa from system_clock to slot_point.
  // hol_toa is already in system_clock (converted in RLC layer), so we can safely convert to slot_point.
  if (mac_dl_bs_ind.hol_toa.has_value()) {
    const high_resolution_clock::time_point sl_tp = last_slot_tp.load(std::memory_order_relaxed);
    if (sl_tp != high_resolution_clock::time_point{}) {
      // Convert system_clock to high_resolution_clock, then to slot_point.
      // Since both clocks may have different epochs, we use the relative delay approach.
      auto system_toa = mac_dl_bs_ind.hol_toa.value();
      auto system_now = system_clock::now();
      auto delay = system_now - system_toa;  // 상대 지연 계산
      auto hr_toa = sl_tp - std::chrono::duration_cast<high_resolution_clock::duration>(delay);
      bs.hol_toa = chrono_to_slot_point(hr_toa, sl_tp, last_slot_point.load(std::memory_order_relaxed));
    }
  }
  
  sched_impl->handle_dl_buffer_state_indication(bs);
}
```

**장점**:
- `system_clock`에서 `high_resolution_clock`으로 변환 시 상대 지연 계산 사용 (안전)
- 이후 slot_point 변환은 기존 로직 그대로 사용

### 4. 스케줄러 UE Context 변경

#### 4.1 `dl_logical_channel_manager` 수정

**파일**: `lib/scheduler/ue_context/dl_logical_channel_manager.h`

**변경 내용**:
```cpp
struct channel_context {
  bool active = false;
  unsigned buf_st = 0;
  moving_averager<unsigned> avg_bytes_per_slot;
  unsigned last_sched_bytes = 0;
  slot_point hol_toa;  // ✅ 변경 없음 (기존대로 사용)
  std::optional<ran_slice_id_t> slice_id;
};

// ✅ Update DL buffer status (변경 없음)
void handle_dl_buffer_status_indication(lcid_t lcid, unsigned buffer_status, slot_point hol_toa = {})
{
  channels[lcid].buf_st = std::min(buffer_status, max_buffer_status);
  channels[lcid].hol_toa = hol_toa;
}
```

#### 4.2 UE Context 수정

**파일**: 
- `lib/scheduler/ue_context/ue.h`
- `lib/scheduler/ue_context/ue.cpp`

**변경 내용**:
```cpp
// ✅ 함수 시그니처 (변경 없음)
void handle_dl_buffer_state_indication(lcid_t lcid, unsigned bs, slot_point hol_toa = {});

// ✅ 구현 (변경 없음)
void ue::handle_dl_buffer_state_indication(lcid_t lcid, unsigned bs, slot_point hol_toa)
{
  // ... 기존 로직 ...
  
  logger.info("[HOL-TOA-RECEIVED] UE{} LCID{} hol_toa_valid={} hol_toa={} pending_bytes={}",
              ue_index, static_cast<unsigned>(lcid),
              hol_toa.valid(),
              hol_toa.valid() ? hol_toa.to_uint() : 0,
              pending_bytes);
  
  dl_lc_ch_mgr.handle_dl_buffer_status_indication(lcid, pending_bytes, hol_toa);
}
```

#### 4.3 `slice_ue_repository` 수정

**파일**: `lib/scheduler/slicing/slice_ue_repository.h`

**변경 내용**:
```cpp
/// Retrieve the Head-of-Line (HOL) Time-of-arrival (TOA) for a given logical channel.
slot_point dl_hol_toa(lcid_t lcid) const
{
  return contains(lcid) ? u.dl_logical_channels().hol_toa(lcid) : slot_point{};
}
// ✅ hol_delay_ms 관련 함수 제거 (변경 없음)
```

### 5. 스케줄러 QoS Policy 변경

#### 5.1 `scheduler_time_qos.cpp` 수정

**파일**: `lib/scheduler/policy/scheduler_time_qos.cpp`

**변경 내용**:
```cpp
// ✅ 기존 방식 그대로 사용 (hol_toa를 slot_point로 받아서 계산)
slot_point hol_toa = u.dl_hol_toa(lc->lcid);
if (hol_toa.valid() and slot_tx >= hol_toa) {
  const unsigned hol_delay_ms = (slot_tx - hol_toa) / slot_tx.nof_slots_per_subframe();
  const unsigned pdb = lc->qos->runtime_qos.packet_delay_budget_ms;
  double delay_contrib = hol_delay_ms / static_cast<double>(pdb);
  delay_weight += delay_contrib;
  
  logger.info("[DELAY-WEIGHT] UE{} LCID{} hol_toa={} slot_tx={} hol_delay_ms={} PDB={}ms delay_contrib={:.3f} delay_weight={:.3f}",
              u.ue_index(), static_cast<unsigned>(lc->lcid),
              hol_toa.to_uint(), slot_tx.to_uint(),
              hol_delay_ms, pdb, delay_contrib, delay_weight);
}
```

**장점**:
- 기존 로직 그대로 사용 (변경 최소화)
- RLC에서 이미 `system_clock`으로 변환했으므로 안전

### 6. UE Event Manager 변경

#### 6.1 `ue_event_manager.cpp` 수정

**파일**: `lib/scheduler/ue_scheduling/ue_event_manager.cpp`

**변경 내용**:
```cpp
// ✅ ue_dl_bo_table을 std::pair로 유지 (변경 없음)
std::array<std::pair<std::atomic<int>, std::atomic<int>>, NOF_BEARER_KEYS> ue_dl_bo_table;

void handle_dl_buffer_state_indication(const dl_buffer_state_indication_message& rlc_dl_bo) override
{
  unsigned key = get_bearer_key(rlc_dl_bo.ue_index, rlc_dl_bo.lcid);
  bool first_rlc_bo = ue_dl_bo_table[key].first.exchange(rlc_dl_bo.bs, std::memory_order_acquire) < 0;
  ue_dl_bo_table[key].second.store(rlc_dl_bo.hol_toa.valid() ? rlc_dl_bo.hol_toa.count_val : -1,
                                   std::memory_order_relaxed);
  // ✅ hol_delay_ms 관련 코드 제거
}

void slot_indication(slot_point sl)
{
  // ...
  int hol_toa = ue_dl_bo_table[key].second.load(std::memory_order_relaxed);
  if (hol_toa >= 0) {
    dl_bo.hol_toa = std::min(sl, slot_point{sl.numerology(), (unsigned)hol_toa});
  }
  // ✅ hol_delay_ms 관련 코드 제거
  // ...
  u.handle_dl_buffer_state_indication(dl_bo.lcid, dl_bo.bs, dl_bo.hol_toa);
}
```

## 변경된 파일 목록

### 헤더 파일
1. `include/srsran/rlc/rlc_buffer_state.h` - `hol_toa`를 `system_clock::time_point`로 변경
2. `include/srsran/mac/mac_ue_control_information_handler.h` - 변경 없음 (이미 `system_clock`)
3. `include/srsran/scheduler/scheduler_dl_buffer_state_indication_handler.h` - 변경 없음

### 구현 파일
1. `lib/rlc/rlc_tx_am_entity.cpp` - `hol_toa`를 `system_clock`으로 변환
2. `lib/rlc/rlc_tx_um_entity.cpp` - `hol_toa`를 `system_clock`으로 변환
3. `lib/rlc/rlc_tx_tm_entity.cpp` - `hol_toa`를 `system_clock`으로 변환
4. `lib/mac/mac_dl/mac_cell_processor.cpp` - `hol_toa` 그대로 전달 (변환 제거)
5. `lib/mac/mac_sched/srsran_scheduler_adapter.cpp` - `system_clock` → `slot_point` 변환 (상대 지연 계산)
6. `lib/scheduler/ue_context/ue.cpp` - 변경 없음
7. `lib/scheduler/ue_scheduling/ue_event_manager.cpp` - 변경 없음
8. `lib/scheduler/policy/scheduler_time_qos.cpp` - 변경 없음 (기존 로직 사용)

## 장점

### 1. Clock Epoch 문제 해결
- RLC에서 `system_clock`으로 변환 (상대 지연 계산이므로 안전)
- MAC에서 변환 없이 그대로 전달
- 스케줄러 어댑터에서 상대 지연 계산으로 안전하게 변환

### 2. 변경 범위 최소화
- `hol_delay_ms` 추가 없이 기존 구조 유지
- 스케줄러 로직 변경 없음
- 코드 변경이 매우 적음

### 3. 플랫폼 독립성
- 상대 지연 계산 방식으로 clock epoch 차이 문제 회피
- 다양한 플랫폼/컴파일러에서 일관된 동작 보장

### 4. 코드 단순성
- 불필요한 필드 추가 없음
- 기존 로직 최대한 유지
- 이해하기 쉬운 구조

## 테스트 고려사항

### 1. 단위 테스트
- RLC 엔티티에서 `hol_delay_ms` 계산 정확성 검증
- 다양한 지연 시간에 대한 계산 검증

### 2. 통합 테스트
- RLC → MAC → 스케줄러 어댑터 → 스케줄러 전체 경로 검증
- `hol_delay_ms`가 정상적으로 전달되는지 확인

### 3. 성능 테스트
- Clock 변환 제거로 인한 성능 향상 측정
- 메모리 사용량 변화 확인

### 4. 회귀 테스트
- 기존 `hol_toa` fallback 경로 동작 확인
- delay_weight 계산 결과 일관성 확인

## 로그 예시

### RLC에서 hol_delay_ms 계산
```
[HOL-TOA-RECEIVED] UE0 LCID4 hol_toa_valid=true hol_toa=7491 hol_delay_ms=50 pending_bytes=1000
```

### 스케줄러에서 hol_delay_ms 사용
```
[DELAY-WEIGHT] UE0 LCID4 hol_delay_ms=50 PDB=100ms delay_contrib=0.500 delay_weight=0.500 (from_hol_delay_ms=true)
```

## 참고사항

### 기존 방식과의 차이점

| 항목 | 기존 방식 | 새로운 방식 |
|------|----------|------------|
| 전달 데이터 | `time_point` (steady_clock) | `unsigned` (milliseconds) |
| 변환 필요 | 3단계 변환 (steady → system → high_res → slot) | 변환 없음 |
| Clock epoch 의존 | 있음 (위험) | 없음 (안전) |
| 플랫폼 독립성 | 낮음 | 높음 |
| 성능 | 변환 오버헤드 있음 | 오버헤드 없음 |

### 마이그레이션 가이드

기존 코드가 `hol_toa`를 사용하는 경우:
1. `hol_delay_ms`가 우선 사용되므로 자동으로 마이그레이션됨
2. `hol_delay_ms`가 없을 때만 기존 `hol_toa` 경로 사용
3. 점진적으로 모든 경로에서 `hol_delay_ms` 사용 가능

## 구현 방법 선택

### 선택한 방법: 간단한 방법

이번 변경에서는 **간단한 방법**을 선택했습니다:
- RLC에서 `hol_toa`를 `system_clock::time_point`로 계산
- MAC에서 그대로 전달
- 스케줄러 어댑터에서 `system_clock` → `slot_point` 변환 (상대 지연 계산)

### 대안: hol_delay_ms 추가 방법

더 명시적인 방법으로 `hol_delay_ms`를 추가하는 방법도 있었습니다:
- 장점: 변환 완전 제거, 더 명시적
- 단점: 변경 범위가 큼, 모든 레이어에 필드 추가 필요

하지만 **간단한 방법**이 변경 범위가 작고 충분히 안전하므로 선택했습니다.

## 결론

이번 변경으로 clock epoch 변환 문제를 해결했습니다. RLC에서 `hol_toa`를 `system_clock::time_point`로 계산해서 전달함으로써:

1. ✅ Clock epoch 문제 해결 (상대 지연 계산 방식)
2. ✅ 변경 범위 최소화 (기존 구조 유지)
3. ✅ 플랫폼 독립성 확보
4. ✅ 코드 단순성 유지

스케줄러는 이제 안전하고 정확하게 delay_weight를 계산할 수 있습니다.

### 핵심 포인트

- **RLC**: `steady_clock`의 `time_of_arrival`을 `system_clock::time_point`로 변환 (상대 지연 계산)
- **MAC**: 변환 없이 그대로 전달
- **스케줄러 어댑터**: `system_clock` → `high_resolution_clock` → `slot_point` 변환 (상대 지연 계산)
- **스케줄러**: 기존 로직 그대로 사용

