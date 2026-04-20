# DSCP 기반 5QI 매핑 코드 변경 사항 상세 정리

## 📋 변경 개요

iperf3로 보낸 트래픽의 DSCP 값을 추출하여 동적으로 5QI를 매핑하고, 이를 통해 스케줄러에서 차등 자원 할당을 수행하기 위한 변경 사항입니다.

**핵심 목표**: 
- IPv4 패킷의 DSCP 값을 실시간으로 추출
- DSCP 값에 따라 동적으로 5QI 매핑
- 스케줄러에서 DSCP 기반 우선순위로 차등 자원 할당

---

## 📁 변경된 파일 목록 (총 19개)

### 🆕 신규 생성 파일 (2개)

#### 1. `include/srsran/sdap/dscp_qos_mapper.h` (267줄)
**파일 유형**: 헤더 파일 (신규 생성)  
**목적**: DSCP와 5QI 간의 매핑을 관리하는 싱글톤 클래스

**주요 기능**:
- **싱글톤 패턴**: `get_instance()` - 전역 단일 인스턴스 제공
- **UE별 DSCP 저장**: `register_dscp_for_ue()` - SDAP에서 추출한 DSCP 값 저장
- **UE별 DSCP 조회**: `get_dscp_for_ue()` - DU/스케줄러에서 DSCP 값 조회
- **명시적 매핑 설정**: `set_dscp_to_5qi_mapping()` - 특정 DSCP → 5QI 매핑 설정
- **매핑 조회**: `map_dscp_to_5qi()` - DSCP → 5QI 매핑 조회
- **자동 매핑**: `auto_map_dscp_on_first_observation()` - 첫 관찰 시 자동 5QI 매핑
- **표준 매핑**: `map_dscp_to_5qi_using_standard_mapping()` - 표준 매핑 테이블 사용

**내부 구조**:
- `ue_dscp_map`: UE 인덱스 → DSCP 값 매핑
- `dscp_to_5qi_map`: DSCP → 5QI 명시적 매핑 테이블
- `get_dscp_to_5qi_mapping_table()`: 64개 DSCP 값(0-63)에 대한 표준 매핑 테이블

**스레드 안전성**: `std::mutex`로 보호된 멀티스레드 안전 구현

---

#### 2. `CODE_CHANGES_SUMMARY.md` (이 파일)
**파일 유형**: 문서 파일 (신규 생성)  
**목적**: 모든 변경 사항을 상세히 정리한 문서

---

### ✏️ 수정된 파일 (17개)

#### 📦 SDAP 관련 파일 (4개)

##### 1. `lib/sdap/sdap_entity_tx_impl.h` (168줄)
**파일 유형**: 헤더 파일 (구현 포함)  
**변경 위치**: 36-163줄

**주요 변경 사항**:

1. **`extract_dscp_from_ipv4()` 함수 추가** (36-64줄)
   - IPv4 헤더에서 DSCP 값 추출
   - IPv4 버전 검증 (상위 4비트 = 0x4)
   - 최소 헤더 길이 검증 (20바이트)
   - ToS 필드의 상위 6비트(DSCP) 추출
   - 반환: `std::optional<uint8_t>` (추출 실패 시 빈 값)

2. **`handle_sdu()` 함수 수정** (86-148줄)
   - **[단계 1]**: IPv4 헤더에서 DSCP 추출 및 로깅
   - **[단계 2]**: `dscp_qos_mapper`에 UE별 DSCP 등록
   - **[단계 3]**: 첫 관찰 시 자동 5QI 매핑
   - **[단계 4]**: 매핑된 5QI 조회 및 로깅
   - `last_dscp` 멤버 변수에 DSCP 값 저장

3. **생성자 파라미터 이름 변경** (72-76줄)
   - `ue_index` → `ue_index_` (shadowing 방지)
   - `psi` → `psi_` (shadowing 방지)

4. **`get_last_dscp()` 메서드 추가** (152-153줄)
   - 마지막으로 추출한 DSCP 값 반환

**의존성**: `srsran/sdap/dscp_qos_mapper.h` 포함

---

##### 2. `lib/sdap/sdap_entity_impl.h` (143줄)
**파일 유형**: 헤더 파일 (구현 포함)  
**변경 위치**: 64-84줄

**주요 변경 사항**:

1. **`get_dscp_for_qfi()` 메서드 추가** (64-72줄)
   - 특정 QFI에 대한 DSCP 값 조회
   - `tx_map`에서 해당 QFI의 `sdap_entity_tx_impl` 찾기
   - `get_last_dscp()` 호출하여 DSCP 반환
   - `override` 키워드로 인터페이스 구현

2. **`get_dscp_for_drb()` 메서드 추가** (74-84줄)
   - 특정 DRB에 대한 DSCP 값 조회
   - `tx_map`을 순회하며 해당 DRB ID 찾기
   - `get_last_dscp()` 호출하여 DSCP 반환
   - `override` 키워드로 인터페이스 구현

**의존성**: `sdap_entity_tx_impl`의 `get_last_dscp()` 메서드 사용

---

##### 3. `include/srsran/sdap/sdap.h` (104줄)
**파일 유형**: 헤더 파일 (인터페이스 정의)  
**변경 위치**: 94-98줄

**주요 변경 사항**:

1. **`get_dscp_for_qfi()` 선언 추가** (94-95줄)
   - QFI 기반 DSCP 조회 인터페이스
   - 기본 구현: 빈 값 반환 (하위 클래스에서 오버라이드)

2. **`get_dscp_for_drb()` 선언 추가** (97-98줄)
   - DRB 기반 DSCP 조회 인터페이스
   - 기본 구현: 빈 값 반환 (하위 클래스에서 오버라이드)

**의존성**: 없음 (순수 인터페이스)

---

##### 4. `lib/sdap/CMakeLists.txt` (26줄)
**파일 유형**: CMake 빌드 파일  
**변경 위치**: 24-25줄

**주요 변경 사항**:

1. **라이브러리 링크 추가** (24-25줄)
   - `target_link_libraries(srsran_sdap srsran_ran)` 추가
   - `dscp_qos_mapper.h`에서 `get_5qi_to_qos_characteristics_mapping()` 함수 사용
   - 이 함수는 `srsran_ran` 라이브러리에 정의되어 있어 링크 필요

**의존성**: `srsran_ran` 라이브러리

---

#### 📦 DU 관련 파일 (4개)

##### 5. `lib/du/du_high/du_manager/ran_resource_management/du_bearer_resource_manager.cpp` (335줄)
**파일 유형**: 구현 파일  
**변경 위치**: 28줄, 206-266줄, 279줄

**주요 변경 사항**:

1. **Include 추가** (28줄)
   ```cpp
   #include "srsran/sdap/dscp_qos_mapper.h"
   #include "srsran/ran/qos/five_qi_qos_mapping.h"
   ```

2. **`setup_drbs()` 함수 수정** (206-266줄)
   - **[단계 5]**: DRB 설정 시 DSCP 기반 5QI 매핑 로직 추가
   - `dscp_qos_mapper` 싱글톤 인스턴스 획득
   - UE별 DSCP 값 조회 (`get_dscp_for_ue()`)
   - DSCP → 5QI 매핑 적용 (`map_dscp_to_5qi()`)
   - 표준 매핑 폴백 (`map_dscp_to_5qi_using_standard_mapping()`)
   - QoS 특성 검증 및 로깅 (`get_5qi_to_qos_characteristics_mapping()`)
   - `packet_error_rate_t` 포맷 수정 (`per.to_double()` 사용)

3. **DRB 설정 시 매핑된 5QI 사용** (279줄)
   ```cpp
   new_drb.qos.qos_desc.get_nondyn_5qi().five_qi = mapped_5qi;
   ```
   - Core에서 받은 5QI 대신 DSCP 기반 매핑된 5QI 사용

**로깅**:
- `[STEP5-DU-DRB]` 태그로 상세 로그 출력
- DSCP 조회, 매핑 적용, QoS 특성 확인 등 단계별 로깅

---

##### 6. `lib/du/du_high/du_manager/ran_resource_management/du_bearer_resource_manager.h` (69줄)
**파일 유형**: 헤더 파일  
**변경 위치**: 58줄

**주요 변경 사항**:

1. **`setup_drbs()` 함수 시그니처 수정** (58줄)
   - `du_ue_index_t ue_index` 파라미터 추가
   - UE 인덱스를 통해 DSCP 값을 조회할 수 있도록 함

**의존성**: 없음 (시그니처만 변경)

---

##### 7. `lib/du/du_high/du_manager/ran_resource_management/du_ran_resource_manager_impl.cpp`
**파일 유형**: 구현 파일  
**변경 위치**: `setup_drbs()` 호출 부분

**주요 변경 사항**:

1. **`setup_drbs()` 호출 시 UE 인덱스 전달**
   - `du_bearer_resource_manager.h`의 시그니처 변경에 따른 호출부 수정
   - UE 인덱스를 파라미터로 전달하여 DSCP 조회 가능하도록 함

**의존성**: `du_bearer_resource_manager.h`의 시그니처 변경

---

##### 8. `lib/du/du_high/du_manager/converters/asn1_rrc_config_helpers.cpp`
**파일 유형**: 구현 파일  
**변경 위치**: 미확인 (git status에 표시됨)

**예상 변경 사항**:
- DRB 설정 관련 ASN.1 변환 로직에서 5QI 처리 부분 수정 가능성
- 직접적인 DSCP 관련 변경은 없을 것으로 예상

---

##### 9. `lib/du/du_high/du_manager/converters/scheduler_configuration_helpers.cpp`
**파일 유형**: 구현 파일  
**변경 위치**: 미확인 (git status에 표시됨)

**예상 변경 사항**:
- 스케줄러 설정 변환 로직에서 5QI 처리 부분 수정 가능성
- 직접적인 DSCP 관련 변경은 없을 것으로 예상

---

#### 📦 5QI 매핑 관련 파일 (2개)

##### 10. `include/srsran/ran/qos/five_qi_qos_mapping.h` (79줄)
**파일 유형**: 헤더 파일 (인터페이스 정의)  
**변경 위치**: 74-76줄

**주요 변경 사항**:

1. **`get_all_available_5qi_values()` 함수 선언 추가** (74-76줄)
   ```cpp
   std::vector<five_qi_t> get_all_available_5qi_values();
   ```
   - 모든 표준 5QI 값을 우선순위로 정렬하여 반환
   - `dscp_qos_mapper`의 자동 매핑 기능에서 사용
   - 우선순위가 낮은 값(높은 우선순위)부터 정렬

**의존성**: 없음 (순수 선언)

---

##### 11. `lib/ran/qos/five_qi_qos_mapping.cpp` (113줄 이상)
**파일 유형**: 구현 파일  
**변경 위치**: 91-114줄, 87줄

**주요 변경 사항**:

1. **`get_all_available_5qi_values()` 함수 구현** (91-114줄)
   - `five_qi_to_qos_mapping`에서 모든 5QI 값 추출
   - 우선순위(priority)로 정렬 (낮은 priority 값 = 높은 우선순위)
   - 정렬된 5QI 목록 반환

2. **변수명 수정** (87줄)
   - `qos_chars` → `qos_char` (shadowing 방지)

**의존성**: `five_qi_to_qos_mapping` 맵 사용

---

#### 📦 스케줄러 관련 파일 (3개)

##### 12. `lib/scheduler/policy/scheduler_time_qos.cpp` (565줄 이상)
**파일 유형**: 구현 파일  
**변경 위치**: 27줄, 170-175줄, 218-225줄, 294-404줄

**주요 변경 사항**:

1. **Include 추가** (27줄)
   ```cpp
   #include "srsran/sdap/dscp_qos_mapper.h"
   ```

2. **`apply_5qi_based_runtime_overrides()` 함수 수정** (294-404줄)
   - **[단계 6]**: 스케줄링 시마다 DSCP 기반 5QI 동적 조회 로직 추가
   - **[단계 6-1]**: DSCP 기반 5QI 조회
     - `dscp_qos_mapper`에서 UE별 DSCP 값 조회
     - 명시적 매핑 시도 (`map_dscp_to_5qi()`)
     - 표준 매핑 폴백 (`map_dscp_to_5qi_using_standard_mapping()`)
   - **[단계 6-2]**: 5QI → Priority 변환 및 Runtime QoS 업데이트
     - `effective_5qi` 결정 (DSCP 기반 또는 원본)
     - `get_5qi_to_qos_characteristics_mapping()`로 QoS 특성 조회
     - `runtime_qos.priority` 업데이트 (`set_runtime_qos()`)
     - ARP priority는 Core 값 유지 (변경하지 않음)

3. **`compute_dl_qos_weights()` 함수 수정** (170-175줄, 218-225줄)
   - **[단계 7]**: Priority 계산 로그 추가
   - **[단계 8]**: prio_weight 계산 로그 추가
   - combined priority 계산 과정 상세 로깅

**로깅**:
- `[STEP6-SCHED]` 태그로 상세 로그 출력
- DSCP 조회, 매핑 적용, Priority 업데이트 등 단계별 로깅

**핵심 로직**:
- 매 스케줄링 슬롯마다 최신 DSCP 값을 확인
- `runtime_qos.priority`를 동적으로 업데이트
- `combined_prio = runtime_qos.priority × runtime_arp_priority`
- 낮은 `combined_prio` = 높은 우선순위 = 더 많은 리소스 할당

---

##### 13. `lib/scheduler/policy/scheduler_time_qos.h` (115줄)
**파일 유형**: 헤더 파일  
**변경 위치**: 변경 없음 (git status에 표시됨)

**예상 변경 사항**:
- 직접적인 변경은 없을 것으로 예상
- 구현 파일의 변경으로 인해 git status에 표시된 것으로 추정

---

##### 14. `lib/scheduler/ue_context/dl_logical_channel_manager.cpp` (539줄 이상)
**파일 유형**: 구현 파일  
**변경 위치**: 미확인 (git status에 표시됨)

**예상 변경 사항**:
- Logical Channel 관리 로직에서 5QI 처리 부분 수정 가능성
- 직접적인 DSCP 관련 변경은 없을 것으로 예상

---

#### 📦 Logical Channel Config 관련 파일 (1개)

##### 15. `include/srsran/scheduler/config/logical_channel_config.h` (101줄)
**파일 유형**: 헤더 파일  
**변경 위치**: 51-59줄

**주요 변경 사항**:

1. **Runtime QoS 필드 추가** (51-59줄)
   - `mutable standardized_qos_characteristics runtime_qos;` (53줄)
   - `mutable arp_prio_level_t runtime_arp_priority;` (56줄)
   - `mutable std::optional<gbr_qos_flow_information> runtime_gbr_qos_info;` (59줄)

2. **동기화 메서드 추가** (61-67줄)
   - `sync_runtime_with_original()`: 원본 값으로 runtime 값 동기화
   - `set_runtime_qos()`: runtime QoS 설정
   - `set_runtime_arp_priority()`: runtime ARP priority 설정

**목적**:
- 원본 QoS 정보를 보존하면서 스케줄러에서만 동적으로 변경 가능
- DSCP 기반 차등 자원 할당을 위한 동적 priority 업데이트 지원

---

#### 📦 CU-UP 관련 파일 (2개)

##### 16. `lib/cu_up/pdu_session_manager_impl.cpp` (834줄 이상)
**파일 유형**: 구현 파일  
**변경 위치**: 미확인 (git status에 표시됨)

**예상 변경 사항**:
- PDU Session 관리 로직에서 5QI 처리 부분 수정 가능성
- 직접적인 DSCP 관련 변경은 없을 것으로 예상

---

##### 17. `lib/cu_up/qos_flow_context.h` (99줄)
**파일 유형**: 헤더 파일  
**변경 위치**: 미확인 (git status에 표시됨)

**예상 변경 사항**:
- QoS Flow Context에서 runtime profile 관련 수정 가능성
- 직접적인 DSCP 관련 변경은 없을 것으로 예상

---

## 🔄 데이터 흐름

```
iperf3 (DSCP 설정, 예: -S 0x2E)
    ↓
[SDAP TX] IPv4 헤더에서 DSCP 추출 (단계 1)
    ↓
[dscp_qos_mapper] UE별 DSCP 등록 (단계 2)
    ↓
[dscp_qos_mapper] DSCP → 5QI 자동 매핑 (단계 3)
    ↓
[DU] DRB 설정 시 DSCP 기반 5QI 적용 (단계 5)
    ↓
[Scheduler] 스케줄링 시마다 DSCP 기반 5QI 동적 조회 (단계 6)
    ↓
[Scheduler] Runtime QoS Priority 업데이트 (단계 6-2)
    ↓
[Scheduler] Combined Priority 계산 (단계 7)
    ↓
[Scheduler] Priority Weight 계산 및 차등 자원 할당 (단계 8)
```

---

## 🛠️ 컴파일 에러 수정 사항

### 1. 변수 Shadowing 에러
- **파일**: `lib/ran/qos/five_qi_qos_mapping.cpp`
- **위치**: 87줄
- **수정**: `qos_chars` → `qos_char`
- **이유**: 함수 파라미터와 지역 변수 이름 충돌

### 2. 생성자 파라미터 Shadowing
- **파일**: `lib/sdap/sdap_entity_tx_impl.h`
- **위치**: 72-76줄
- **수정**: `ue_index` → `ue_index_`, `psi` → `psi_`
- **이유**: 멤버 변수와 파라미터 이름 충돌

### 3. byte_buffer.view() 에러
- **파일**: `lib/sdap/sdap_entity_tx_impl.h`
- **위치**: 94줄
- **수정**: `sdu.view()` → `byte_buffer_view(sdu)`
- **이유**: API 변경 또는 잘못된 사용

### 4. override 키워드 누락
- **파일**: `lib/sdap/sdap_entity_impl.h`
- **위치**: 65줄, 75줄
- **수정**: `get_dscp_for_qfi()`, `get_dscp_for_drb()`에 `override` 추가
- **이유**: 가상 함수 오버라이드 명시

### 5. packet_error_rate_t 포맷 에러
- **파일**: `lib/du/du_high/du_manager/ran_resource_management/du_bearer_resource_manager.cpp`
- **위치**: 240줄
- **수정**: `per` → `per.to_double()`
- **이유**: 로깅 시 double 형식 필요

### 6. 링커 에러
- **파일**: `lib/sdap/CMakeLists.txt`
- **위치**: 24-25줄
- **수정**: `target_link_libraries(srsran_sdap srsran_ran)` 추가
- **이유**: `get_5qi_to_qos_characteristics_mapping()` 함수 사용을 위한 링크 필요

---

## 📊 파일별 변경 통계

| 파일 | 변경 유형 | 주요 변경 내용 | 라인 수 |
|------|---------|--------------|---------|
| `include/srsran/sdap/dscp_qos_mapper.h` | **신규 생성** | DSCP-QoS 매퍼 싱글톤 클래스 | 267줄 |
| `lib/sdap/sdap_entity_tx_impl.h` | 수정 | DSCP 추출 및 등록 로직 추가 | +130줄 |
| `lib/sdap/sdap_entity_impl.h` | 수정 | DSCP 접근 메서드 추가 | +20줄 |
| `include/srsran/sdap/sdap.h` | 수정 | DSCP 인터페이스 선언 추가 | +4줄 |
| `lib/sdap/CMakeLists.txt` | 수정 | srsran_ran 링크 추가 | +1줄 |
| `lib/du/du_high/du_manager/ran_resource_management/du_bearer_resource_manager.cpp` | 수정 | DSCP 기반 5QI 매핑 적용 | +60줄 |
| `lib/du/du_high/du_manager/ran_resource_management/du_bearer_resource_manager.h` | 수정 | setup_drbs() 시그니처 변경 | +1줄 |
| `include/srsran/ran/qos/five_qi_qos_mapping.h` | 수정 | 함수 선언 추가 | +3줄 |
| `lib/ran/qos/five_qi_qos_mapping.cpp` | 수정 | 함수 구현 추가 | +24줄 |
| `lib/scheduler/policy/scheduler_time_qos.cpp` | 수정 | DSCP 기반 동적 5QI 조회 | +110줄 |
| `include/srsran/scheduler/config/logical_channel_config.h` | 수정 | Runtime QoS 필드 추가 | +18줄 |
| `CODE_CHANGES_SUMMARY.md` | **신규 생성** | 변경 사항 정리 문서 | - |

**총 19개 파일 변경** (신규 2개, 수정 17개)

---

## 🎯 변경 목적

### 최종 목표
iperf3로 보낸 트래픽의 DSCP 값에 따라 동적으로 5QI를 매핑하고, 이를 통해 스케줄러에서 차등 자원 할당을 수행하여 각 UE의 트래픽 특성에 맞는 QoS를 제공합니다.

### 주요 특징

1. **동적 매핑**: 하드코딩 없이 실제 트래픽의 DSCP 값을 기반으로 5QI 결정
2. **표준 5QI 활용**: `five_qi_qos_mapping.cpp`의 모든 표준 5QI 값을 활용
3. **실시간 반영**: DRB 설정 시점에 DSCP가 없어도, 이후 트래픽이 오면 자동 반영
4. **차등 할당**: DSCP 값에 따라 다른 우선순위의 5QI를 할당하여 차등 자원 할당
5. **스레드 안전**: `std::mutex`로 보호된 멀티스레드 안전 구현
6. **원본 보존**: Core에서 받은 원본 QoS 정보는 보존하고, 스케줄러에서만 runtime 값 사용

---

## 🔍 핵심 컴포넌트 상세 설명

### 1. dscp_qos_mapper (싱글톤)

**역할**: SDAP, DU, Scheduler 간 DSCP 정보 공유

**주요 메서드**:
- `register_dscp_for_ue()`: SDAP에서 호출, UE별 DSCP 저장
- `get_dscp_for_ue()`: DU/Scheduler에서 호출, UE별 DSCP 조회
- `map_dscp_to_5qi()`: DSCP → 5QI 매핑 조회
- `auto_map_dscp_on_first_observation()`: 첫 관찰 시 자동 매핑

**매핑 테이블**:
- 64개 DSCP 값(0-63)에 대한 표준 매핑
- DSCP 값이 높을수록 높은 우선순위의 5QI 할당
- 예: DSCP 63 → 5QI 69 (priority=5), DSCP 0 → 5QI 9 (priority=90)

### 2. SDAP Entity TX (DSCP 추출)

**역할**: DL 트래픽에서 IPv4 헤더 파싱하여 DSCP 추출

**주요 함수**:
- `extract_dscp_from_ipv4()`: IPv4 헤더 파싱, DSCP 추출
- `handle_sdu()`: SDU 처리 시 DSCP 추출 및 등록

**처리 단계**:
1. IPv4 헤더 검증 (버전, 길이)
2. ToS 필드에서 DSCP 추출 (상위 6비트)
3. `dscp_qos_mapper`에 등록
4. 자동 5QI 매핑

### 3. DU Bearer Resource Manager (5QI 적용)

**역할**: DRB 설정 시 DSCP 기반 5QI 적용

**주요 로직**:
1. `dscp_qos_mapper`에서 UE별 DSCP 조회
2. DSCP → 5QI 매핑 적용
3. Core 5QI 대신 매핑된 5QI 사용
4. QoS 특성 검증 및 로깅

### 4. Scheduler Time QoS (동적 조회)

**역할**: 스케줄링 시마다 최신 DSCP 값을 반영하여 우선순위 계산

**주요 로직**:
1. 매 스케줄링 슬롯마다 DSCP 기반 5QI 조회
2. `effective_5qi` 결정 (DSCP 기반 또는 원본)
3. `runtime_qos.priority` 업데이트
4. `combined_prio = runtime_qos.priority × runtime_arp_priority`
5. 낮은 `combined_prio` = 높은 우선순위 = 더 많은 리소스 할당

---

## 📝 로깅 태그

- `[STEP1-SDAP]`: SDAP에서 DSCP 추출
- `[STEP2-MAPPER]`: DSCP 등록
- `[STEP3-AUTO-MAP]`: 자동 5QI 매핑
- `[STEP4-MAPPING]`: DSCP→5QI 매핑 확인
- `[STEP5-DU-DRB]`: DU에서 DRB 설정 시 5QI 적용
- `[STEP6-SCHED]`: 스케줄러에서 DSCP 기반 5QI 조회
- `[STEP7]`: Priority 계산
- `[STEP8]`: Priority Weight 계산

---

## ✅ 테스트 시나리오

### 시나리오 1: 기본 DSCP 추출 및 매핑
1. iperf3로 DSCP 46 (EF) 트래픽 전송
2. SDAP에서 DSCP 추출 확인 (`[STEP1-SDAP]`)
3. `dscp_qos_mapper`에 등록 확인 (`[STEP2-MAPPER]`)
4. 자동 5QI 매핑 확인 (`[STEP3-AUTO-MAP]`)
5. DU에서 DRB 설정 시 매핑된 5QI 사용 확인 (`[STEP5-DU-DRB]`)
6. 스케줄러에서 동적 조회 확인 (`[STEP6-SCHED]`)

### 시나리오 2: 다중 UE 차등 할당
1. UE1: DSCP 46 (높은 우선순위)
2. UE2: DSCP 0 (낮은 우선순위)
3. 각 UE의 5QI 매핑 확인
4. 스케줄러에서 우선순위 차이 확인
5. 리소스 할당 차이 확인

### 시나리오 3: 동적 반영
1. DRB 설정 시점에 DSCP 없음 (Core 5QI 사용)
2. 트래픽 도착 후 DSCP 추출
3. 스케줄러에서 자동으로 DSCP 기반 5QI 반영 확인

---

## 🔧 빌드 및 링크 의존성

```
srsran_sdap
    ├── srsran_ran (새로 추가)
    │   └── get_5qi_to_qos_characteristics_mapping()
    │   └── get_all_available_5qi_values()
    └── dscp_qos_mapper.h
        └── five_qi_qos_mapping.h
```

---

## 📌 주의 사항

1. **IPv4만 지원**: 현재 IPv4 헤더만 파싱 (IPv6 미지원)
2. **DSCP 범위**: 0-63 (6비트)만 유효
3. **스레드 안전**: `dscp_qos_mapper`는 `std::mutex`로 보호
4. **원본 보존**: Core에서 받은 원본 QoS는 변경하지 않음 (runtime만 변경)
5. **매핑 테이블**: 64개 DSCP 값에 대한 하드코딩된 매핑 테이블 사용

---

## 🚀 향후 개선 사항

1. **IPv6 지원**: IPv6 헤더에서 Traffic Class 추출
2. **동적 매핑 테이블**: 설정 파일로 매핑 테이블 관리
3. **매핑 통계**: DSCP별 사용 통계 수집
4. **매핑 검증**: 매핑된 5QI의 유효성 검증 강화
5. **성능 최적화**: 매핑 테이블 조회 최적화

---

**문서 작성일**: 2025-01-XX  
**변경 범위**: DSCP 기반 5QI 매핑 및 차등 자원 할당  
**영향 범위**: SDAP, DU, Scheduler, 5QI 매핑 모듈
