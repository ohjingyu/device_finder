# Device_finder

같은 네트워크(게이트웨이와 같은 `/24` 대역)에서 **웹 첫 화면의 문구로 기종을 판별해 기기를 찾아 주는** Windows 프로그램입니다.
찾은 기기는 표로 보여 주고, IP를 클릭하면 `http://<IP>` 로 브라우저(Chrome)에서 바로 열립니다.

- 설치 불필요 — `Device_Finder.exe` 한 개 + `devices.txt` 한 개
- 의존성 없음 (Win32 API + Winsock, 정적 링크)

## 사용 방법

1. `devices.example.txt` 를 **`devices.txt`** 로 복사해서 `Device_Finder.exe` 와 같은 폴더에 둡니다.
2. `devices.txt` 를 찾으려는 기기에 맞게 수정합니다. ([기종 판별 규칙](#기종-판별-규칙-devicestxt))
3. `Device_Finder.exe` 를 실행하면 자동으로 검색합니다.
4. 검색이 끝나면 "모든 기기의 사이트를 여시겠습니까?" 창이 뜹니다.
   - **예**: 찾은 기기 전부를 엽니다.
   - **아니요**: 목록에서 원하는 IP 를 한 번 클릭해 하나씩 엽니다.
5. **다시 검색 / 모두 열기** 버튼도 있습니다.

## 기종 판별 규칙 (`devices.txt`)

한 줄에 한 규칙, 형식은 다음과 같습니다.

```
기기 첫 화면(GET /)의 HTML에 들어 있는 문자열 = 목록에 표시할 이름
```

```
# 예시
Example Device A Console = Device-A
Example Device B Web Setup = Device-B
```

- 각 IP 의 80 포트에 `GET /` 한 번을 보내고, 응답 앞부분(2KB)에 왼쪽 문자열이 들어 있으면 오른쪽 이름의 기기로 판정합니다.
- 보통 첫 화면의 `<title>` 문구를 쓰면 됩니다. 대소문자를 구분합니다.
- `#` 로 시작하는 줄은 주석, 파일은 UTF-8, 최대 32줄입니다.
- `devices.txt` 는 `.gitignore` 에 포함되어 있어 저장소에 올라가지 않습니다. (현장/회사 전용 값은 이 파일에만 둡니다.)

## 안전

기기에 보내는 요청은 `GET /` **한 번**뿐입니다. 재부팅, 설정 변경, 업데이트 같은 동작 경로는 호출하지 않습니다.
다만 포트 검색이 제한된 보안 환경에서는 사용 전에 허용 여부를 확인하세요.

## 빌드

MinGW-w64(gcc, windres)가 필요합니다.

```
windres app.rc -O coff -o app_res.o
gcc -O2 -mwindows -static device_finder.c app_res.o -o Device_Finder.exe -liphlpapi -lws2_32 -lcomctl32
```

## 알려진 제한

- 검색 범위는 게이트웨이와 같은 `/24` 대역(`x.x.x.1~254`) 고정입니다.
- 80 포트 HTTP 만 지원합니다. (HTTPS, 다른 포트 미지원)
- 서명되지 않은 실행 파일이라 Windows 보호 화면(SmartScreen)이 뜰 수 있습니다. **추가 정보 → 실행** 을 누르세요.
- VPN/가상 어댑터가 있으면 다른 대역을 검색할 수 있습니다. 화면 위쪽에 표시되는 대역을 확인하세요.
