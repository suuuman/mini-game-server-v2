# scripts\harness_common.ps1 — 8종 하네스가 "예약 경유"로 마을에 붙기 위한 공용 기구
#
#   dot-source 로 쓴다:
#     . (Join-Path $PSScriptRoot 'harness_common.ps1')
#
#   kLoginReq 가 폐지되고 kEnterReq(예약 소비)만 남은 뒤로는, 예전처럼
#   회귀 하네스가 마을에 맨몸으로 붙어 곧장 kJoinZoneReq 를 쏘는 접속 방식은
#   전부 거부당한다. 그런데 예약은 "세션 서버"만 발급할 수 있고, 이 하네스들은
#   존·거래·인벤토리 같은 마을 내부 로직을 재는 것이지 세션 배정을 재는 게
#   아니다(그건 session.ps1 95항목이 이미 덮는다) — 그래서 실물 session.exe 를
#   띄우지 않고, 이 파일이 "가짜 세션 서버" 노릇을 해서 마을에 직접 Reserve 를
#   찔러 넣는다.
#
#   새로 발명하지 않는다 — 출처는 둘이다.
#     scripts\s2s.ps1     : 가짜 세션 서버의 8B 헤더 코덱 · TcpListener · New-ReserveBody
#     scripts\session.ps1 : ini 사본 패치의 이중 단언(Set-IniKeyInSection/Assert-IniValue) ·
#                           프로세스 스폰(Start-ServerProcess) · 홈 생성(New-VillageHome) ·
#                           클라 4B 헤더 코덱(Send-ClientFrame/Read-ClientFrame) ·
#                           kEnterReq/kEnterAck 코덱
#   값과 구조를 그대로 옮겨 온다. 리터럴이 같은 동안은 find_copies.ps1 이
#   proto 쪽 값이 바뀌었을 때 이 파일도 걸러 준다.

$ErrorActionPreference = 'Stop'

# ── 프로세스 스폰 — Win32 CreateProcess 를 직접 부른다(.NET Process.Start 우회) ──
#   실측: 이 환경에서 .NET 의 Process.Start(RedirectStandardInput=$true) 로
#   village.exe 를 띄우면, stdout/stderr 를 전혀 안 읽어도 getchar() 가 즉시
#   EOF 로 깨어난다(3 초를 기다려도 그렇다 — Debug·Release 둘 다, S2S 켜짐·꺼짐
#   둘 다 재현). 반면 같은 방식으로 띄운 findstr.exe 는 stdin 을 정상적으로
#   기다린다 — .NET 리다이렉션 자체가 이 환경에서 깨진 게 아니라, .NET 의
#   Process 클래스가 세 스트림(stdin·stdout·stderr)을 동시에 리다이렉트할 때
#   내부적으로 만드는 파이프·핸들 설정 절차가 village.exe 의 초기 기동(짧은
#   시간에 스레드를 여럿 띄우는 것)과 맞물려 stdin 파이프의 부모쪽 write 핸들을
#   조기에 잃게 만드는 것으로 보인다(재현은 확정 · 정확한 내부 동작은 미확정).
#   Win32 CreateProcess 를 직접 불러 stdin 은 우리가 만든 파이프로, stdout·
#   stderr 는 파일로 바로 연결하면(=.NET 의 파이프 매개 자체를 안 거치면)
#   getchar() 가 정상적으로 블록하고 개행에 정상 반응한다 — 실측으로 확인.
if (-not ([System.Management.Automation.PSTypeName]'MiniGameHarness.Native').Type) {
    Add-Type -Namespace MiniGameHarness -Name Native -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)]
public struct SECURITY_ATTRIBUTES {
    public int nLength;
    public IntPtr lpSecurityDescriptor;
    public bool bInheritHandle;
}
[StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
public struct STARTUPINFO {
    public int cb;
    public string lpReserved;
    public string lpDesktop;
    public string lpTitle;
    public int dwX; public int dwY; public int dwXSize; public int dwYSize;
    public int dwXCountChars; public int dwYCountChars;
    public int dwFillAttribute; public int dwFlags;
    public short wShowWindow; public short cbReserved2;
    public IntPtr lpReserved2;
    public IntPtr hStdInput; public IntPtr hStdOutput; public IntPtr hStdError;
}
[StructLayout(LayoutKind.Sequential)]
public struct PROCESS_INFORMATION {
    public IntPtr hProcess; public IntPtr hThread; public int dwProcessId; public int dwThreadId;
}
[DllImport("kernel32.dll", EntryPoint = "CreateProcessW", SetLastError = true, CharSet = CharSet.Unicode)]
public static extern bool CreateProcess(
    string lpApplicationName, System.Text.StringBuilder lpCommandLine,
    IntPtr lpProcessAttributes, IntPtr lpThreadAttributes,
    bool bInheritHandles, uint dwCreationFlags, IntPtr lpEnvironment,
    string lpCurrentDirectory, ref STARTUPINFO lpStartupInfo,
    out PROCESS_INFORMATION lpProcessInformation);
[DllImport("kernel32.dll", SetLastError = true)]
public static extern bool CreatePipe(out IntPtr hReadPipe, out IntPtr hWritePipe, ref SECURITY_ATTRIBUTES lpPipeAttributes, uint nSize);
[DllImport("kernel32.dll", EntryPoint = "CreateFileW", SetLastError = true, CharSet = CharSet.Unicode)]
public static extern IntPtr CreateFile(string lpFileName, uint dwDesiredAccess, uint dwShareMode,
    ref SECURITY_ATTRIBUTES lpSecurityAttributes, uint dwCreationDisposition, uint dwFlagsAndAttributes, IntPtr hTemplateFile);
[DllImport("kernel32.dll", SetLastError = true)]
public static extern bool SetHandleInformation(IntPtr hObject, uint dwMask, uint dwFlags);
[DllImport("kernel32.dll", SetLastError = true)]
public static extern bool CloseHandle(IntPtr hObject);
'@
}

# ── proto 상수 — S2S 는 s2s.ps1 과, 클라 규약은 packet.h 와 값으로 맞춘다 ──────
$Harness_MsgRegister      = 0x8001
$Harness_MsgHeartbeat     = 0x8003
$Harness_MsgPlayerEnter   = 0x8004
$Harness_MsgPlayerLeave   = 0x8005
$Harness_MsgDrainComplete = 0x8007
$Harness_MsgReserve       = 0x8101
$Harness_MsgKick          = 0x8102
$Harness_MsgSetMode       = 0x8103
$Harness_MsgRegisterAck   = 0x8201
$Harness_MsgHeartbeatAck  = 0x8203
$Harness_MsgReserveAck    = 0x8301
$Harness_MsgKickAck       = 0x8302
$Harness_MsgSetModeAck    = 0x8303

$Harness_VilEnterReq = 13    # proto::MsgId::kEnterReq  — body: [ player_id : u64 ]
$Harness_VilEnterAck = 114   # proto::MsgId::kEnterAck  — body: [ result:u8 ][ player_id:u64 ][ session_id:u32 ]
$Harness_VilPingReq  = 11    # proto::MsgId::kPingReq   — body 없음(게이트 앞 스위치 — 인증 세션 생존 확인용)
$Harness_VilPongAck  = 111   # proto::MsgId::kPongAck   — body 없음
$Harness_ResultOk    = 0     # proto::ResultCode::kOk
$Harness_ResultBusy  = 4     # proto::ResultCode::kBusy — 드레인 중 Enter 거절 값(D3)
$Harness_ResultInvalidArg = 3   # proto::ResultCode::kInvalidArg — 예약 없음/만료 거절 값(D7b)

# ── S2S 8B 헤더 [body_size:u16][msg_id:u16][seq:u32] — 빅엔디언(s2s.ps1 과 동일) ──
function New-S2sHeader([int]$BodySize, [int]$MsgId, [uint32]$Seq) {
    $h = [byte[]]::new(8)
    $h[0] = [byte](($BodySize -shr 8) -band 0xFF)
    $h[1] = [byte]( $BodySize         -band 0xFF)
    $h[2] = [byte](($MsgId    -shr 8) -band 0xFF)
    $h[3] = [byte]( $MsgId            -band 0xFF)
    $h[4] = [byte](($Seq -shr 24) -band 0xFF)
    $h[5] = [byte](($Seq -shr 16) -band 0xFF)
    $h[6] = [byte](($Seq -shr 8)  -band 0xFF)
    $h[7] = [byte]( $Seq          -band 0xFF)
    return $h
}

function ConvertTo-S2sHeader([byte[]]$Bytes) {
    $bodySize = ([int]$Bytes[0] -shl 8) -bor [int]$Bytes[1]
    $msgId    = ([int]$Bytes[2] -shl 8) -bor [int]$Bytes[3]
    $seq      = ([uint32]$Bytes[4] -shl 24) -bor ([uint32]$Bytes[5] -shl 16) `
                -bor ([uint32]$Bytes[6] -shl 8) -bor [uint32]$Bytes[7]
    [pscustomobject]@{ BodySize = $bodySize; MsgId = $msgId; Seq = $seq }
}

# 빈 배열·1요소 배열을 그냥 return 하면 PowerShell 파이프라인이 "풀어서"
#   내보낸다 — 요소가 0개면 내보낼 게 없어 호출자가 배열이 아니라 $null 을
#   받는다(s2s.ps1 이 실측으로 잡은 함정). 콤마 연산자로 감싸 언롤링을 막는다.
function Read-ExactBytes([System.Net.Sockets.NetworkStream]$Stream, [int]$Count, [int]$TimeoutMs) {
    if ($Count -eq 0) { return ,[byte[]]::new(0) }
    $buf = [byte[]]::new($Count)
    $got = 0
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($got -lt $Count) {
        if ($Stream.DataAvailable) {
            $n = $Stream.Read($buf, $got, $Count - $got)
            if ($n -le 0) { return $null }      # 상대가 닫았다
            $got += $n
        } elseif ($sw.ElapsedMilliseconds -gt $TimeoutMs) {
            return $null                         # 시간 안에 안 왔다
        } else {
            Start-Sleep -Milliseconds 10
        }
    }
    return ,$buf
}

function Read-S2sFrame($Stream, [int]$TimeoutMs) {
    $header = Read-ExactBytes $Stream 8 $TimeoutMs
    if ($null -eq $header) { return $null }
    $h = ConvertTo-S2sHeader $header
    $body = Read-ExactBytes $Stream $h.BodySize $TimeoutMs
    if ($null -eq $body) { return $null }
    return [pscustomobject]@{ MsgId = $h.MsgId; Seq = $h.Seq; Body = $body }
}

function Send-S2sFrame($Stream, [int]$MsgId, [uint32]$Seq, [byte[]]$Body) {
    $header = New-S2sHeader $Body.Length $MsgId $Seq
    $frame = $header + $Body
    $Stream.Write($frame, 0, $frame.Length)
    $Stream.Flush()
}

# 기대한 msg_id 가 아니면 전부 버리는 일반형이다 — 특정 알림(FullSync 등)을
#   이름으로 걸러내는 특수형으로 짜면 안 적힌 알림이 새로 생길 때마다 또
#   깨진다. 다만 Heartbeat 만은 예외로 그 자리에서 회신한다 —
#   ack 를 안 하면 마을이 이 S2S 링크를 미응답으로 보고 스스로 끊기 때문이다.
#   버린 프레임의 msg_id 는 $SkippedIds 에 쌓아 호출부가 진단에 쓸 수 있게 한다.
function Wait-S2sFrame($Stream, [int]$WantMsgId, [int]$TotalTimeoutMs,
        [System.Collections.Generic.List[int]]$SkippedIds) {
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $TotalTimeoutMs) {
        $remain = $TotalTimeoutMs - [int]$sw.ElapsedMilliseconds
        $frame = Read-S2sFrame $Stream ([Math]::Min(500, [Math]::Max(50, $remain)))
        if ($null -eq $frame) { continue }
        if ($frame.MsgId -eq $WantMsgId) { return $frame }
        if ($frame.MsgId -eq $Harness_MsgHeartbeat) {
            Send-S2sFrame $Stream $Harness_MsgHeartbeatAck $frame.Seq ([byte[]]::new(0))
        }
        if ($null -ne $SkippedIds) { $SkippedIds.Add($frame.MsgId) }
    }
    return $null
}

function New-RegisterAckBody([uint32]$ServerId, [byte]$Result) {
    $b = [byte[]]::new(5)
    $b[0] = [byte](($ServerId -shr 24) -band 0xFF); $b[1] = [byte](($ServerId -shr 16) -band 0xFF)
    $b[2] = [byte](($ServerId -shr 8)  -band 0xFF); $b[3] = [byte]( $ServerId          -band 0xFF)
    $b[4] = $Result
    return ,$b
}

# Reserve body: [ player_id : u64 ][ expire_ms : u32 ] (12B)
function New-ReserveBody([uint64]$PlayerId, [uint32]$ExpireMs) {
    $b = [byte[]]::new(12)
    for ($i = 0; $i -lt 8; $i++) { $b[$i] = [byte](($PlayerId -shr (8 * (7 - $i))) -band 0xFF) }
    $b[8]  = [byte](($ExpireMs -shr 24) -band 0xFF); $b[9]  = [byte](($ExpireMs -shr 16) -band 0xFF)
    $b[10] = [byte](($ExpireMs -shr 8)  -band 0xFF); $b[11] = [byte]( $ExpireMs          -band 0xFF)
    return ,$b
}

function ConvertFrom-ReserveAckBody([byte[]]$Body) {
    [pscustomobject]@{ Result = [int]$Body[0] }
}

# SetMode body: [ mode : u8 ] (1B) — 0=Running · 1=Draining
function New-SetModeBody([byte]$Mode) {
    return ,[byte[]]@($Mode)
}

# SetModeAck body: [ current : u32 ] (4B)
function ConvertFrom-SetModeAckBody([byte[]]$Body) {
    $current = ([uint32]$Body[0] -shl 24) -bor ([uint32]$Body[1] -shl 16) `
        -bor ([uint32]$Body[2] -shl 8) -bor [uint32]$Body[3]
    [pscustomobject]@{ Current = $current }
}

# DrainComplete body: [ remaining : u32 ] (4B) — 알림(seq=0), 마을이 자진 발신
function ConvertFrom-DrainCompleteBody([byte[]]$Body) {
    $remaining = ([uint32]$Body[0] -shl 24) -bor ([uint32]$Body[1] -shl 16) `
        -bor ([uint32]$Body[2] -shl 8) -bor [uint32]$Body[3]
    [pscustomobject]@{ Remaining = $remaining }
}

function New-S2sListener([int]$Port) {
    for ($attempt = 1; $attempt -le 20; $attempt++) {
        try {
            $l = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, $Port)
            $l.Server.SetSocketOption(
                [System.Net.Sockets.SocketOptionLevel]::Socket,
                [System.Net.Sockets.SocketOptionName]::ReuseAddress, $true)
            $l.Start()
            return $l
        } catch {
            if ($attempt -eq 20) { throw }
            Start-Sleep -Milliseconds 100
        }
    }
}

function Wait-Accept($Listener, [int]$TimeoutMs, [string]$Label) {
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while (-not $Listener.Pending()) {
        if ($sw.ElapsedMilliseconds -gt $TimeoutMs) {
            Write-Host "  ($Label — ${TimeoutMs}ms 안에 연결이 안 왔다)" -ForegroundColor Yellow
            return $null
        }
        Start-Sleep -Milliseconds 20
    }
    return $Listener.AcceptTcpClient()
}

# ── 클라 4B 헤더 [body_size:u16][msg_id:u16] — packet.h 와 값으로 맞춘다 ──────
function Send-ClientFrame($Stream, [int]$MsgId, [byte[]]$Body) {
    $f = [byte[]]::new(4 + $Body.Length)
    $f[0] = [byte](($Body.Length -shr 8) -band 0xFF)
    $f[1] = [byte]( $Body.Length         -band 0xFF)
    $f[2] = [byte](($MsgId -shr 8) -band 0xFF)
    $f[3] = [byte]( $MsgId         -band 0xFF)
    if ($Body.Length -gt 0) { [Array]::Copy($Body, 0, $f, 4, $Body.Length) }
    $Stream.Write($f, 0, $f.Length)
    $Stream.Flush()
}

function Read-ClientFrame($Stream, [int]$TimeoutMs) {
    $header = Read-ExactBytes $Stream 4 $TimeoutMs
    if ($null -eq $header) { return $null }
    $bodySize = ([int]$header[0] -shl 8) -bor [int]$header[1]
    $msgId    = ([int]$header[2] -shl 8) -bor [int]$header[3]
    $body = Read-ExactBytes $Stream $bodySize $TimeoutMs
    if ($null -eq $body) { return $null }
    return [pscustomobject]@{ MsgId = $msgId; Body = $body }
}

# kEnterReq body: [ player_id : u64 ]
function New-EnterBody([uint64]$PlayerId) {
    $b = [byte[]]::new(8)
    for ($i = 0; $i -lt 8; $i++) { $b[$i] = [byte](($PlayerId -shr (8 * (7 - $i))) -band 0xFF) }
    return ,$b
}

# kEnterAck body: [ result:u8 ][ player_id:u64 ][ session_id:u32 ] (13B)
function ConvertFrom-EnterAckBody([byte[]]$Body) {
    $result = [int]$Body[0]
    $playerId = [uint64]0
    for ($i = 0; $i -lt 8; $i++) { $playerId = ($playerId -shl 8) -bor [uint64]$Body[1 + $i] }
    $sessionId = ([uint32]$Body[9] -shl 24) -bor ([uint32]$Body[10] -shl 16) `
                -bor ([uint32]$Body[11] -shl 8) -bor [uint32]$Body[12]
    [pscustomobject]@{ Result = $result; PlayerId = $playerId; SessionId = $sessionId }
}

# ── ini 사본 패치 — 키마다 정확히 1줄 치환 + 재읽기 대조(조용한 실패 방지) ──
#    s2s.ps1·session.ps1 과 값·구조가 같다 — 세 곳이 각자 인라인으로 들고 있던
#    것을 여기 하나로 모을 수도 있었지만, 그건 이번 상한(하네스 신규 1·수정 10)
#    밖의 리팩터라 손대지 않는다. 새로 여는 이 파일만 이 사본을 쓴다.
function Set-IniKeyInSection([string]$Path, [string]$Section, [string]$Key, [string]$Value) {
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    $text = [System.Text.Encoding]::UTF8.GetString($bytes).TrimStart([char]0xFEFF)
    $lines = $text -split "`n"
    $inSection = $false
    $replaced = 0
    $keyPattern = '^\s*' + [regex]::Escape($Key) + '\s*='

    for ($i = 0; $i -lt $lines.Length; $i++) {
        $line = $lines[$i]
        if ($line -match '^\s*\[(.+?)\]\s*$') {
            $inSection = ($Matches[1].Trim() -eq $Section)
            continue
        }
        if ($inSection -and ($line -match $keyPattern)) {
            $pad = ' ' * [Math]::Max(1, 21 - $Key.Length)
            $lines[$i] = "$Key$pad= $Value"
            $replaced++
        }
    }

    if ($replaced -ne 1) {
        throw "설정 치환 실패 — [$Section] $Key 는 정확히 1줄이어야 하는데 ${replaced}줄 매치됐다"
    }

    $newText = [string]::Join("`n", $lines)
    $utf8Bom = New-Object System.Text.UTF8Encoding($true)
    [System.IO.File]::WriteAllText($Path, $newText, $utf8Bom)
}

function Assert-IniValue([string]$Path, [string]$Section, [string]$Key, [string]$Expected) {
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    $text = [System.Text.Encoding]::UTF8.GetString($bytes).TrimStart([char]0xFEFF)
    $lines = $text -split "`n"
    $inSection = $false
    $found = $null
    $keyPattern = '^\s*' + [regex]::Escape($Key) + '\s*=\s*(.*?)\s*$'
    foreach ($line in $lines) {
        if ($line -match '^\s*\[(.+?)\]\s*$') {
            $inSection = ($Matches[1].Trim() -eq $Section)
            continue
        }
        if ($inSection -and ($line -match $keyPattern)) {
            $found = $Matches[1]
            break
        }
    }
    if ($found -ne $Expected) {
        throw "설정 재검증 실패 — [$Section] $Key 기대값 '$Expected' 실제 '$found'"
    }
}

# stdout/stderr 는 파일로 직접 잇는다(위 CreateProcess 주석 근거) — proc.stdout.log
#   가 곧 리다이렉트 대상이라 별도 드레인·사후 회수가 필요 없다. 반환값은 진짜
#   System.Diagnostics.Process(GetProcessById) 라 .Kill()·.HasExited·.ExitCode·
#   .WaitForExit()·.MainWindowTitle(zone_race.ps1 의 assert 대화상자 판정)가
#   기존 호출부 그대로 동작한다 — 다만 stdin 쓰기용 스트림은 이 인스턴스가 스스로
#   연 게 아니라서 .StandardInput 이 없다. NoteProperty 로 옆에 매달아 Stop-Harness
#   가 그것으로 개행을 보낸다(호출부 8곳의 시그니처는 손대지 않는다).
function Start-ServerProcess([string]$ExePath, [string]$HomeDir, [string]$Arguments) {
    $sa = New-Object MiniGameHarness.Native+SECURITY_ATTRIBUTES
    $sa.nLength = [System.Runtime.InteropServices.Marshal]::SizeOf([type][MiniGameHarness.Native+SECURITY_ATTRIBUTES])
    $sa.bInheritHandle = $true
    $sa.lpSecurityDescriptor = [IntPtr]::Zero

    $stdinRead = [IntPtr]::Zero
    $stdinWrite = [IntPtr]::Zero
    if (-not [MiniGameHarness.Native]::CreatePipe([ref]$stdinRead, [ref]$stdinWrite, [ref]$sa, 0)) {
        throw "CreatePipe(stdin) 실패: $([System.Runtime.InteropServices.Marshal]::GetLastWin32Error())"
    }
    # 부모가 쥐는 쪽(write)은 자식에 상속되지 않게 한다 — 상속되면 자식이 죽어도
    #   부모가 쥔 사본이 파이프를 열어 둬 EOF 가 안 난다.
    [void][MiniGameHarness.Native]::SetHandleInformation($stdinWrite, 1, 0)

    $GENERIC_WRITE = 0x40000000
    $FILE_SHARE_RW = 0x3
    $CREATE_ALWAYS = 2
    $FILE_ATTRIBUTE_NORMAL = 0x80
    $stdoutHandle = [MiniGameHarness.Native]::CreateFile((Join-Path $HomeDir 'proc.stdout.log'),
        $GENERIC_WRITE, $FILE_SHARE_RW, [ref]$sa, $CREATE_ALWAYS, $FILE_ATTRIBUTE_NORMAL, [IntPtr]::Zero)
    $stderrHandle = [MiniGameHarness.Native]::CreateFile((Join-Path $HomeDir 'proc.stderr.log'),
        $GENERIC_WRITE, $FILE_SHARE_RW, [ref]$sa, $CREATE_ALWAYS, $FILE_ATTRIBUTE_NORMAL, [IntPtr]::Zero)
    if ($stdoutHandle -eq [IntPtr]-1 -or $stderrHandle -eq [IntPtr]-1) {
        $err = [System.Runtime.InteropServices.Marshal]::GetLastWin32Error()
        [void][MiniGameHarness.Native]::CloseHandle($stdinRead)
        [void][MiniGameHarness.Native]::CloseHandle($stdinWrite)
        if ($stdoutHandle -ne [IntPtr]-1) { [void][MiniGameHarness.Native]::CloseHandle($stdoutHandle) }
        if ($stderrHandle -ne [IntPtr]-1) { [void][MiniGameHarness.Native]::CloseHandle($stderrHandle) }
        throw "CreateFile(proc.stdout/stderr.log) 실패: $err"
    }

    $si = New-Object MiniGameHarness.Native+STARTUPINFO
    $si.cb = [System.Runtime.InteropServices.Marshal]::SizeOf([type][MiniGameHarness.Native+STARTUPINFO])
    $si.dwFlags = 0x100   # STARTF_USESTDHANDLES
    $si.hStdInput = $stdinRead
    $si.hStdOutput = $stdoutHandle
    $si.hStdError = $stderrHandle

    $pi = New-Object MiniGameHarness.Native+PROCESS_INFORMATION
    $cmdLine = New-Object System.Text.StringBuilder("`"$ExePath`" $Arguments")
    $ok = [MiniGameHarness.Native]::CreateProcess($ExePath, $cmdLine, [IntPtr]::Zero, [IntPtr]::Zero,
        $true, 0, [IntPtr]::Zero, $HomeDir, [ref]$si, [ref]$pi)
    if (-not $ok) {
        $err = [System.Runtime.InteropServices.Marshal]::GetLastWin32Error()
        [void][MiniGameHarness.Native]::CloseHandle($stdinRead)
        [void][MiniGameHarness.Native]::CloseHandle($stdinWrite)
        [void][MiniGameHarness.Native]::CloseHandle($stdoutHandle)
        [void][MiniGameHarness.Native]::CloseHandle($stderrHandle)
        throw "CreateProcess 실패($ExePath): $err"
    }
    # 자식에 넘긴 핸들의 부모쪽 사본을 닫는다 — 자식은 상속받은 자기 사본을 쥔다.
    [void][MiniGameHarness.Native]::CloseHandle($stdinRead)
    [void][MiniGameHarness.Native]::CloseHandle($stdoutHandle)
    [void][MiniGameHarness.Native]::CloseHandle($stderrHandle)
    [void][MiniGameHarness.Native]::CloseHandle($pi.hThread)
    # hProcess 는 여기서 안 닫는다 — GetProcessById 가 PID 로 다시 찾는데, 이 핸들을
    #   쥐고 있는 동안은 OS 가 그 PID 를 다른 프로세스에 재할당하지 못한다(핸들이
    #   프로세스 객체를 참조로 붙들기 때문). 먼저 닫으면 그 사이(TOCTOU 창)에 같은
    #   PID 가 재사용된 경우 GetProcessById 가 엉뚱한 프로세스를 가리킬 수 있다.
    try {
        $proc = [System.Diagnostics.Process]::GetProcessById($pi.dwProcessId)
    } catch {
        [void][MiniGameHarness.Native]::CloseHandle($stdinWrite)
        [void][MiniGameHarness.Native]::CloseHandle($pi.hProcess)
        throw "자식이 기동 직후 종료됐다(pid=$($pi.dwProcessId)) — $HomeDir\proc.stderr.log 확인. 원 예외: $($_.Exception.Message)"
    }
    [void][MiniGameHarness.Native]::CloseHandle($pi.hProcess)   # GetProcessById 가 제 핸들을 새로 연다 — 이제 닫아도 안전하다
    $safeStdin = New-Object Microsoft.Win32.SafeHandles.SafeFileHandle($stdinWrite, $true)
    $stdinStream = New-Object System.IO.FileStream($safeStdin, [System.IO.FileAccess]::Write)
    # FileStream 은 WriteLine 이 없다 — Process.StandardInput 과 같은 모양(StreamWriter)
    #   으로 감싼다. AutoFlush 는 꺼 둔다 — Stop-Harness 가 WriteLine 뒤 명시적으로
    #   Flush 하므로 두 번 쓸 이유가 없다(App.StandardInput 의 기본값과 동일).
    $stdinWriter = New-Object System.IO.StreamWriter($stdinStream)
    $proc | Add-Member -MemberType NoteProperty -Name HarnessStdin -Value $stdinWriter -Force
    return $proc
}

# ── 하네스 홈 — village.exe 가 읽을 config 사본. [s2s] host 를 켜서 이 파일의
#    가짜 세션 서버(SessionPort)를 보게 하는 것 말고는 원본과 같다(과설계
#    방지 — 나머지 절은 커밋본 기본값을 그대로 믿는다). ────────────────────
function New-HarnessHome([string]$ScratchRoot, [string]$ClientPort = '9000', [string]$SessionPort = '9100') {
    $root = Split-Path -Parent $PSScriptRoot
    $cfgDir = Join-Path $ScratchRoot 'config'
    New-Item -ItemType Directory -Path $cfgDir -Force | Out-Null
    $dst = Join-Path $cfgDir 'server.ini'
    Copy-Item -LiteralPath (Join-Path $root 'config\server.ini') -Destination $dst

    $patchServer = [ordered]@{ port = $ClientPort }
    foreach ($k in $patchServer.Keys) { Set-IniKeyInSection $dst 'server' $k $patchServer[$k] }

    # 커밋값(config/server.ini [net] idle_timeout_sec)이 90 으로 올라간 뒤에도
    #   이 9종은 ping 을 안 보내는 하네스라 켜 두면 그 자체로 끊긴다 — 절 이름은
    #   [net] 이다([server] 아님. 세션 서버 쪽 [server] 와 헷갈리기 쉬운 자리).
    $patchNet = [ordered]@{ idle_timeout_sec = '0' }
    foreach ($k in $patchNet.Keys) { Set-IniKeyInSection $dst 'net' $k $patchNet[$k] }

    # s2s.ps1 이 이미 검증해 둔 값 그대로다(§3-1 축소 설정값 유효 범위 표 —
    #   [s2s] 4키는 [1, 3'600'000] 안이면 된다). heartbeat_ms·backoff 를 굳이
    #   더 줄이지 않는다 — 이 하네스들은 예약 발급 왕복만 S2S 를 쓰고, 그
    #   왕복이 끝난 뒤 링크가 유휴 상태로 남아도(또는 오래 걸리는 시나리오
    #   도중 하트비트 무응답으로 마을이 재연결을 시도해도) 이미 성사된
    #   Enter·게임 세션에는 영향이 없다.
    $patchS2s = [ordered]@{
        host                 = '127.0.0.1'
        port                 = $SessionPort
        backoff_initial_ms   = '250'
        backoff_max_ms       = '1000'
        request_timeout_ms   = '1500'
        heartbeat_ms         = '500'
    }
    foreach ($k in $patchS2s.Keys) { Set-IniKeyInSection $dst 's2s' $k $patchS2s[$k] }

    foreach ($k in $patchServer.Keys) { Assert-IniValue $dst 'server' $k $patchServer[$k] }
    foreach ($k in $patchNet.Keys) { Assert-IniValue $dst 'net' $k $patchNet[$k] }
    foreach ($k in $patchS2s.Keys) { Assert-IniValue $dst 's2s' $k $patchS2s[$k] }

    return $ScratchRoot
}

# $Seconds 는 호출부 8곳이 여전히 넘긴다(각자 하네스 시나리오 전체를 덮는 여유 값으로
#   주석에 살아 있다) — 여기서 손대지 않고 죽은 인자로 받기만 한다. 정상 종료가
#   stdin 개행(Stop-Harness)으로 옮겨져 village 는 이제 getchar 대기 경로로만 스폰되므로
#   --seconds 는 더 이상 넘기지 않는다(auto_seconds>0 이면 main.cpp:195-201 가 getchar 를
#   아예 안 불러 stdin 종료가 먹히지 않는다). 호출부 8곳까지 정리하면 이번 Step 의 대상
#   밖(harness_common.ps1·s2s.ps1·session.ps1) 7개 스크립트에 손이 번져 상한을 넘긴다.
function Start-Village([string]$Config, [string]$HomeDir, [int]$Seconds = 120) {
    $root = Split-Path -Parent $PSScriptRoot
    $exe = Join-Path $root "build\x64\$Config\village.exe"
    if (-not (Test-Path $exe)) { throw "$exe 가 없다 — 먼저 .\scripts\build.ps1 로 빌드하라" }
    # 고아 방지 안전망이 --seconds 에서 Stop-Harness 의 「finally Kill 폴백」으로 옮겨졌다
    #   (위 주석) — 하네스가 도중 강제 종료돼도 다음 실행이 여기서 잔존 프로세스를 회수한다.
    #   s2s.ps1:278 과 같은 패턴이다.
    Get-Process village -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 300
    if ($Config -eq 'ASan') {
        # ASan 빌드는 clang_rt.asan_dynamic-x86_64.dll 에 의존하는데 그 DLL 이 MSVC
        # 도구 폴더에만 있고 PATH 에 없다. 못 찾으면 village 가 로그를 한 줄도 안
        # 남기고 시작조차 못 하는데, 하네스에는 「S2S 연결이 오지 않았다」로만 보여
        # 원인과 증상이 멀다. 사전 기동 서버를 쓰던 시절에는 사람이 run-asan.ps1 로
        # 띄웠으므로 이 자리가 필요 없었다.
        # run-asan.ps1·session.ps1 과 같은 리터럴이다. 세 곳이 각자 골격이라 공용
        # 함수로 묶이지 않으므로 find_copies.ps1 이 동기화를 지킨다.
        $dll = Get-ChildItem 'C:\Program Files\Microsoft Visual Studio\*\*\VC\Tools\MSVC\*\bin\Hostx64\x64\clang_rt.asan_dynamic-x86_64.dll' -ErrorAction SilentlyContinue |
               Sort-Object -Property FullName -Descending | Select-Object -First 1
        if ($dll) {
            $env:PATH = "$($dll.DirectoryName);$env:PATH"
            Write-Host "asan  : $($dll.DirectoryName)"
        } else {
            Write-Host "asan  : DLL 을 못 찾음 — 그냥 실행해 봅니다" -ForegroundColor Yellow
        }
        $env:ASAN_OPTIONS = 'abort_on_error=0'
    }
    return Start-ServerProcess $exe $HomeDir ''
}

# 리스너를 village 스폰보다 먼저 세운다 — s2s.ps1 은 반대 순서(리스너가 없는
#   상태에서 재시도 수열을 관찰)를 일부러 택했지만, 이 하네스들은 백오프를
#   재는 게 아니라 그냥 빨리 등록되기를 바란다. 리스너가 먼저 있으면 village 의
#   첫 ConnectEx 가 곧바로 성공해 s2s.ps1 이 겪은 재시도 사전 대기(수 초)가
#   통째로 없어진다.
function Start-FakeSession([int]$Port) {
    return New-S2sListener $Port
}

# village 의 최초 S2S 연결을 받아 Register/RegisterAck 를 주고받고, 등록 직후
#   마을이 스스로 먼저 보내는 FullSync(집합이 비어도 chunk_total=1 로 한 번)를
#   걷어낸 뒤 그 스트림을 돌려준다. 이후 Grant-Reservation 이 같은 스트림 위에
#   Reserve 를 계속 실어 보낸다 — S2S 연결은 프로세스당 하나뿐이라 재활용한다.
function Accept-FakeSessionLink($Listener, [int]$TimeoutMs = 8000) {
    $client = Wait-Accept $Listener $TimeoutMs 'village S2S 등록'
    if ($null -eq $client) { throw '가짜 세션 서버 — village 의 S2S 연결이 오지 않았다' }
    $stream = $client.GetStream()
    $reg = Read-S2sFrame $stream 3000
    if ($null -eq $reg -or $reg.MsgId -ne $Harness_MsgRegister) {
        throw '가짜 세션 서버 — Register 를 못 받았다(또는 msg_id 가 다르다)'
    }
    Send-S2sFrame $stream $Harness_MsgRegisterAck $reg.Seq (New-RegisterAckBody 1 0)
    # FullSync 는 이 시나리오의 관심사가 아니다 — 다음 대기 자리에 끼어 오답이
    #   되지 않도록 여기서 한 번 걷어내고 버린다(Wait-S2sFrame 의 일반 필터링
    #   원칙과 같은 이유로, 첫 프레임만은 순서가 확정적이라 이렇게 앞서 처리한다).
    $null = Read-S2sFrame $stream 2000
    return [pscustomobject]@{ Client = $client; Stream = $stream; Seq = [uint32]1 }
}

# $Link 는 Accept-FakeSessionLink 가 돌려준 객체다. Reserve 마다 seq 를 1씩
#   올린다 — Register 가 이미 seq=1 을 썼으므로 2부터 시작한다.
function Grant-Reservation($Link, [uint64]$PlayerId, [uint32]$ExpireMs = 10000) {
    $Link.Seq = $Link.Seq + [uint32]1
    Send-S2sFrame $Link.Stream $Harness_MsgReserve $Link.Seq (New-ReserveBody $PlayerId $ExpireMs)
    $skipped = New-Object System.Collections.Generic.List[int]
    $ack = Wait-S2sFrame $Link.Stream $Harness_MsgReserveAck 4000 $skipped
    if ($null -eq $ack) {
        $skipText = if ($skipped.Count -gt 0) { ($skipped | ForEach-Object { '0x{0:X4}' -f $_ }) -join ',' } else { '(없음)' }
        throw "예약 발급 실패 — player=$PlayerId 의 ReserveAck 를 못 받았다 (그사이 버린 프레임=$skipText)"
    }
    $body = ConvertFrom-ReserveAckBody $ack.Body
    if ($body.Result -ne 0) {
        throw "예약 발급 거부됨 — player=$PlayerId result=$($body.Result)"
    }
}

# SetMode 발신 → SetModeAck 수신까지 — Grant-Reservation 과 같은 왕복 형태다.
#   드레인 하네스(drain.ps1) 전용 — 8종 자체 스폰 하네스는 드레인을 안 쓴다.
function Set-VillageMode($Link, [bool]$Draining) {
    $Link.Seq = $Link.Seq + [uint32]1
    $mode = if ($Draining) { [byte]1 } else { [byte]0 }
    Send-S2sFrame $Link.Stream $Harness_MsgSetMode $Link.Seq (New-SetModeBody $mode)
    $skipped = New-Object System.Collections.Generic.List[int]
    $ack = Wait-S2sFrame $Link.Stream $Harness_MsgSetModeAck 4000 $skipped
    if ($null -eq $ack) {
        $skipText = if ($skipped.Count -gt 0) { ($skipped | ForEach-Object { '0x{0:X4}' -f $_ }) -join ',' } else { '(없음)' }
        throw "SetMode 응답 실패 — draining=$Draining 의 SetModeAck 를 못 받았다 (그사이 버린 프레임=$skipText)"
    }
    return ConvertFrom-SetModeAckBody $ack.Body
}

# Kick body: [ player_id : u64 ][ reason : u8 ] (9B) — proto::s2s::Kick 과 값으로 맞춘다
function New-KickBody([uint64]$PlayerId, [byte]$Reason) {
    $b = [byte[]]::new(9)
    for ($i = 0; $i -lt 8; $i++) { $b[$i] = [byte](($PlayerId -shr (8 * (7 - $i))) -band 0xFF) }
    $b[8] = $Reason
    return ,$b
}

function ConvertFrom-KickAckBody([byte[]]$Body) {
    [pscustomobject]@{ Result = [int]$Body[0] }
}

# Kick 발신 → KickAck 수신까지 — Set-VillageMode 와 같은 왕복 형태다. reason 은
#   proto::s2s::kKickReasonDuplicate(0) 고정 — 지금 값이 그것뿐이다.
#   Skipped 를 반환에 함께 싣는다 — 이 왕복이 기다리는 동안 PlayerLeave 같은
#   알림이 먼저 도착하면 Wait-S2sFrame 이 그 프레임을 조용히 버리는데(관심
#   msg_id 만 반환), 호출부가 그걸 모른 채 뒤이어 같은 msg_id 를 다시
#   기다리면 오지도 않은 걸 기다리는 플레이크가 된다. 버린 msg_id 목록을
#   그대로 돌려주면 호출부가 먼저 이 목록을 보고, 없을 때만 새로 기다릴 수
#   있다(trade.ps1 DupTest 의 PlayerLeave 확인 참조).
function Send-VillageKick($Link, [uint64]$PlayerId) {
    $Link.Seq = $Link.Seq + [uint32]1
    Send-S2sFrame $Link.Stream $Harness_MsgKick $Link.Seq (New-KickBody $PlayerId 0)
    $skipped = New-Object System.Collections.Generic.List[int]
    $ack = Wait-S2sFrame $Link.Stream $Harness_MsgKickAck 4000 $skipped
    if ($null -eq $ack) {
        $skipText = if ($skipped.Count -gt 0) { ($skipped | ForEach-Object { '0x{0:X4}' -f $_ }) -join ',' } else { '(없음)' }
        throw "Kick 응답 실패 — player=$PlayerId 의 KickAck 를 못 받았다 (그사이 버린 프레임=$skipText)"
    }
    $body = ConvertFrom-KickAckBody $ack.Body
    return [pscustomobject]@{ Result = $body.Result; Skipped = $skipped }
}

# Connect-Reserved 와 달리 kEnterAck 의 result 를 던지지 않고 그대로 돌려준다 —
#   드레인 시나리오(D3·D7·D7b)는 성공이 아니라 거절값 자체가 검증 대상이라
#   Connect-Reserved 의 throw-on-reject 계약이 안 맞는다. 연결도 실패 시에
#   안 닫는다 — D3 가 kBusy 거절 뒤에도 "연결 유지"를 그 자리에서 계속 써야
#   한다(같은 연결을 D7 이 재사용한다).
function Send-VilEnter([int]$ClientPort, [uint64]$PlayerId, [int]$TimeoutMs = 5000) {
    $client = New-Object System.Net.Sockets.TcpClient
    $client.NoDelay = $true
    $client.Connect('127.0.0.1', $ClientPort)
    $stream = $client.GetStream()
    $stream.ReadTimeout = $TimeoutMs
    $stream.WriteTimeout = $TimeoutMs

    Send-ClientFrame $stream $Harness_VilEnterReq (New-EnterBody $PlayerId)
    $ack = Read-ClientFrame $stream $TimeoutMs
    if ($null -eq $ack -or $ack.MsgId -ne $Harness_VilEnterAck) {
        return [pscustomobject]@{ Client = $client; Stream = $stream; Result = -1; PlayerId = $PlayerId; SessionId = 0 }
    }
    $body = ConvertFrom-EnterAckBody $ack.Body
    return [pscustomobject]@{
        Client = $client; Stream = $stream
        Result = $body.Result; PlayerId = $body.PlayerId; SessionId = $body.SessionId
    }
}

# 이미 연결된 스트림 위에서 kEnterReq 를 다시 보낸다(D7 이 D3 의 연결을
#   재사용한다) — Send-VilEnter 는 매번 새 TcpClient 를 여니 이 자리에는 안 맞는다.
function Send-VilEnterAgain($Stream, [uint64]$PlayerId, [int]$TimeoutMs = 5000) {
    Send-ClientFrame $Stream $Harness_VilEnterReq (New-EnterBody $PlayerId)
    $ack = Read-ClientFrame $Stream $TimeoutMs
    if ($null -eq $ack -or $ack.MsgId -ne $Harness_VilEnterAck) {
        return [pscustomobject]@{ Result = -1; PlayerId = $PlayerId; SessionId = 0 }
    }
    $body = ConvertFrom-EnterAckBody $ack.Body
    return [pscustomobject]@{ Result = $body.Result; PlayerId = $body.PlayerId; SessionId = $body.SessionId }
}

# D4 의 "기존 접속이 정상인가" 확인용 — kPingReq 는 body 가 없고 인증 세션의
#   생존만 보면 되므로, 다인원 브로드캐스트가 필요한 채팅보다 이 쪽이 최소
#   도구다(zone.ps1 은 브로드캐스트 자체를 재는 게 목적이라 그쪽을 쓰지만
#   여기는 아니다).
function Test-VilAlive($Stream, [int]$TimeoutMs = 3000) {
    Send-ClientFrame $Stream $Harness_VilPingReq ([byte[]]::new(0))
    $ack = Read-ClientFrame $Stream $TimeoutMs
    return ($null -ne $ack) -and ($ack.MsgId -eq $Harness_VilPongAck)
}

# Grant-Reservation → 마을 9000 connect → kEnterReq → kEnterAck 확인까지
#   한 번에 끝내고 접속 핸들을 돌려준다. 반환 모양은 기존 하네스들의
#   "Connect-*" 류 헬퍼(zone.ps1 의 $cli 원소 · trade.ps1 의 $c 등)와
#   맞추기 쉽도록 Client/Stream 을 최상위에 둔다.
function Connect-Reserved($Link, [int]$ClientPort, [uint64]$PlayerId, [int]$TimeoutMs = 5000) {
    Grant-Reservation $Link $PlayerId

    $client = New-Object System.Net.Sockets.TcpClient
    $client.NoDelay = $true
    $client.Connect('127.0.0.1', $ClientPort)
    $stream = $client.GetStream()
    $stream.ReadTimeout = $TimeoutMs
    $stream.WriteTimeout = $TimeoutMs

    Send-ClientFrame $stream $Harness_VilEnterReq (New-EnterBody $PlayerId)
    $ack = Read-ClientFrame $stream $TimeoutMs
    if ($null -eq $ack -or $ack.MsgId -ne $Harness_VilEnterAck) {
        $client.Close()
        throw "Enter 실패 — player=$PlayerId 의 kEnterAck 를 못 받았다"
    }
    $body = ConvertFrom-EnterAckBody $ack.Body
    if ($body.Result -ne $Harness_ResultOk) {
        $client.Close()
        throw "Enter 거부됨 — player=$PlayerId result=$($body.Result)"
    }
    return [pscustomobject]@{
        Client = $client; Stream = $stream
        PlayerId = $body.PlayerId; SessionId = $body.SessionId
    }
}

function Stop-Harness([string]$ScratchRoot, $Listener, $Proc) {
    try { if ($Listener) { $Listener.Stop() } } catch {}
    # 개행으로 정상 종료를 먼저 시도한다 — 그래야 [ALLOC][POOL ][CONN ][NET  ][TICK ] 종료
    #   지표와 ASan 의 exit-time 검사를 얻는다. assert 대화상자에 갇힌 워커/틱 스레드는
    #   workers.stop()/tick.stop() 의 join 을 영원히 막으므로 WaitForExit 이 그 갈래를
    #   잡아내고, 그때만 Kill 로 폴백한다(정상 경로는 여기서 안 걸린다).
    try {
        if ($Proc -and -not $Proc.HasExited) {
            # .StandardInput 이 아니다 — Start-ServerProcess 주석 참조(Win32
            #   CreateProcess 로 띄운 인스턴스라 .NET 이 열어 준 스트림이 없다).
            $Proc.HarnessStdin.WriteLine('')
            $Proc.HarnessStdin.Flush()
            $Proc.HarnessStdin.Close()
            if (-not $Proc.WaitForExit(5000)) {
                $Proc.Kill()
            }
        }
    } catch {
        try { if ($Proc -and -not $Proc.HasExited) { $Proc.Kill() } } catch {}
    }
    Start-Sleep -Milliseconds 300
    try { Remove-Item -Recurse -Force -LiteralPath $ScratchRoot -ErrorAction SilentlyContinue } catch {}
}
