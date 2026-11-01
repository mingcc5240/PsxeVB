VERSION 5.00
Begin VB.Form Form1 
   Caption         =   "Form1"
   ClientHeight    =   9660
   ClientLeft      =   120
   ClientTop       =   465
   ClientWidth     =   10740
   LinkTopic       =   "Form1"
   ScaleHeight     =   9660
   ScaleWidth      =   10740
   StartUpPosition =   3  'Windows 기본값
   Begin VB.Timer Timer1 
      Enabled         =   0   'False
      Interval        =   500
      Left            =   9720
      Top             =   8400
   End
   Begin VB.CommandButton cmdSelectRom 
      Caption         =   "LoadRom"
      Height          =   375
      Left            =   2040
      TabIndex        =   3
      Top             =   8760
      Width           =   1455
   End
   Begin VB.CommandButton cmdSelectBios 
      Caption         =   "Bios"
      Height          =   375
      Left            =   360
      TabIndex        =   2
      Top             =   8760
      Width           =   1455
   End
   Begin VB.PictureBox picScreen 
      Height          =   7920
      Left            =   120
      ScaleHeight     =   524
      ScaleMode       =   0  '사용자
      ScaleWidth      =   640
      TabIndex        =   1
      Top             =   120
      Width           =   8295
   End
   Begin VB.CommandButton cmdStop 
      Caption         =   "Stop"
      Height          =   495
      Left            =   4080
      TabIndex        =   0
      Top             =   8640
      Width           =   1695
   End
End
Attribute VB_Name = "Form1"
Attribute VB_GlobalNameSpace = False
Attribute VB_Creatable = False
Attribute VB_PredeclaredId = True
Attribute VB_Exposed = False
Option Explicit

' =============================================================================
' 1. psxe.dll API 선언 (-Wl,--kill-at 옵션을 사용하여 빌드한 기준)
' =============================================================================
Private Declare Function PSXE_Init Lib "psxe.dll" ( _
    ByVal hWnd As Long, _
    ByVal biosPath As String, _
    ByVal expPath As String, _
    ByVal cdPath As String, _
    ByVal exePath As String _
) As Long

Private Declare Sub PSXE_LoadCD Lib "psxe.dll" (ByVal cdPath As String)
Private Declare Function PSXE_RunFrame Lib "psxe.dll" () As Long
Private Declare Sub PSXE_SetPadButton Lib "psxe.dll" (ByVal buttonMask As Long, ByVal isPressed As Long)
Private Declare Sub PSXE_Command Lib "psxe.dll" (ByVal cmdId As Long)
Private Declare Sub PSXE_Shutdown Lib "psxe.dll" ()
Private Declare Function PSXE_GetFPS Lib "psxe.dll" () As Single

' =============================================================================
' 2. OCX 없이 파일 열기 창을 띄우기 위한 Win32 API (comdlg32.dll) 선언
' =============================================================================
Private Type OPENFILENAME
    lStructSize       As Long
    hwndOwner         As Long
    hInstance         As Long
    lpstrFilter       As String
    lpstrCustomFilter As String
    nMaxCustFilter    As Long
    nFilterIndex      As Long
    lpstrFile         As String
    nMaxFile          As Long
    lpstrFileTitle    As String
    nMaxFileTitle     As Long
    lpstrInitialDir   As String
    lpstrTitle        As String
    Flags             As Long
    nFileOffset       As Integer
    nFileExtension    As Integer
    lpstrDefExt       As String
    lCustData         As Long
    lpfnHook          As Long
    lpTemplateName    As String
End Type

Private Declare Function GetOpenFileName Lib "comdlg32.dll" Alias "GetOpenFileNameA" (pOpenfilename As OPENFILENAME) As Long
Private Const OFN_FILEMUSTEXIST As Long = &H1000&
Private Const OFN_PATHMUSTEXIST As Long = &H800&
Private Const OFN_HIDEREADONLY  As Long = &H4&
Private Const OFN_NOCHANGEDIR   As Long = &H8&

' =============================================================================
' 3. PS1 패드 버튼 비트마스크 상수 (psxe sda.h 기준)
' =============================================================================
Private Const PSXI_SW_SDA_SELECT    As Long = &H1&
Private Const PSXI_SW_SDA_L3        As Long = &H2&
Private Const PSXI_SW_SDA_R3        As Long = &H4&
Private Const PSXI_SW_SDA_START     As Long = &H8&
Private Const PSXI_SW_SDA_PAD_UP    As Long = &H10&
Private Const PSXI_SW_SDA_PAD_RIGHT As Long = &H20&
Private Const PSXI_SW_SDA_PAD_DOWN  As Long = &H40&
Private Const PSXI_SW_SDA_PAD_LEFT  As Long = &H80&
Private Const PSXI_SW_SDA_L2        As Long = &H100&
Private Const PSXI_SW_SDA_R2        As Long = &H200&
Private Const PSXI_SW_SDA_L1        As Long = &H400&
Private Const PSXI_SW_SDA_R1        As Long = &H800&
Private Const PSXI_SW_SDA_TRIANGLE  As Long = &H1000&
Private Const PSXI_SW_SDA_CIRCLE    As Long = &H2000&
Private Const PSXI_SW_SDA_CROSS     As Long = &H4000&
Private Const PSXI_SW_SDA_SQUARE    As Long = &H8000&

' =============================================================================
' 4. 폼 내부 상태 변수
' =============================================================================
Private m_bRunning  As Boolean
Private m_sBiosPath As String
Private m_sRomPath  As String

Private Sub Form_Load()
    ' [핵심] 프로젝트 폴더(App.Path)를 현재 작업 디렉터리로 고정하여 psxe.dll과 SDL2.dll을 찾게 함
    ChDrive App.Path
    ChDir App.Path
    
    ' 폼 및 컨트롤 레이아웃 자동 설정 (픽셀 단위)
    Me.ScaleMode = vbPixels
    Me.Caption = "PSXe VB6 Frontend (F1: VRAM뷰 / F3: 회전 / F4: 필터 / F5: 리셋)"
    Me.KeyPreview = True  ' 버튼에 포커스가 있어도 폼이 먼저 키 입력을 받음
    
    ' 화면 출력용 PictureBox 설정 (640x480)
    picScreen.Move 8, 44, 640, 480
    picScreen.BackColor = vbBlack
    picScreen.BorderStyle = 1
    
    ' 상단 버튼 배치
    cmdSelectBios.Move 8, 8, 160, 28
    cmdSelectBios.Caption = "BIOS: 내장 (SCPH1001)"
    
    cmdSelectRom.Move 176, 8, 220, 28
    cmdSelectRom.Caption = "게임(CD/EXE) 열기 및 시작"
    
    cmdStop.Move 404, 8, 110, 28
    cmdStop.Caption = "정지 (Stop)"
    cmdStop.Enabled = False
    
    ' 폼 전체 크기 맞춤
    Me.Width = (640 + 32) * Screen.TwipsPerPixelX
    Me.Height = (480 + 96) * Screen.TwipsPerPixelY
    
    ' [수정] 기본값은 빈 문자열("")로 두어 psxe.dll의 내장 BIOS(scph1001.h)를 자동 사용
    m_sBiosPath = vbNullString
    
    Timer1.Interval = 500
    Timer1.Enabled = True
End Sub

' 1. 외부 BIOS 파일 선택 버튼 (선택 사항: 다른 바이오스로 바꾸고 싶을 때만 사용)
Private Sub cmdSelectBios_Click()
    Dim sFile As String
    sFile = ShowOpenDialog("PS1 BIOS 파일 (*.bin;*.rom)|*.bin;*.rom|모든 파일 (*.*)|*.*", "외부 PS1 BIOS 파일 선택 (취소 시 내장 BIOS 사용)")
    
    If sFile <> "" Then
        m_sBiosPath = sFile
        cmdSelectBios.Caption = "외부: " & GetFileName(sFile)
    Else
        m_sBiosPath = vbNullString
        cmdSelectBios.Caption = "BIOS: 내장 (SCPH1001)"
    End If
End Sub

' 2. ROM(CD 이미지 또는 PS-EXE) 선택 및 에뮬레이터 실행 버튼
Private Sub cmdSelectRom_Click()
    ' [수정] 내장 BIOS가 있으므로 m_sBiosPath가 비어있어도 경고창 없이 바로 진행합니다!
    Dim sFile As String
    sFile = ShowOpenDialog( _
        "PS1 게임 파일 (*.cue;*.bin;*.iso;*.exe)|*.cue;*.bin;*.iso;*.exe|" & _
        "BIOS로만 부팅 (취소 버튼 클릭)|*.none|모든 파일 (*.*)|*.*", _
        "실행할 PS1 게임(CUE/BIN/EXE) 선택 (취소 시 BIOS만 부팅)")
    
    ' 이미 실행 중이면 디스크 교체(Swap Disc)만 수행
    If m_bRunning Then
        If sFile <> "" Then
            m_sRomPath = sFile
            PSXE_LoadCD sFile
            Me.Caption = "PSXe 실행 중 - " & GetFileName(sFile)
        End If
        Exit Sub
    End If
    
    m_sRomPath = sFile
    StartEmulator
End Sub

' 에뮬레이터 초기화 및 메인 루프
Private Sub StartEmulator()
    Dim sCdPath  As String
    Dim sExePath As String
    Dim lRes     As Long
    
    ' 선택한 파일 확장자가 .exe 이면 PS-EXE로 로드, 아니면 CD-ROM 이미지로 로드
    If LCase$(Right$(m_sRomPath, 4)) = ".exe" Then
        sCdPath = vbNullString
        sExePath = m_sRomPath
    Else
        sCdPath = m_sRomPath
        sExePath = vbNullString
    End If
    
    ' psxe.dll 초기화 호출 (m_sBiosPath가 ""이면 DLL이 자동으로 내장 scph1001.h를 로드함)
    lRes = PSXE_Init(picScreen.hWnd, m_sBiosPath, vbNullString, sCdPath, sExePath)
    
    Select Case lRes
        Case 0
            ' 정상 초기화 완료
        Case 1
            MsgBox "BIOS 초기화에 실패했습니다.", vbCritical, "BIOS 오류"
            Exit Sub
        Case 2
            MsgBox "EXP1 확장 ROM 초기화에 실패했습니다.", vbCritical, "EXP1 오류"
            Exit Sub
        Case Else
            MsgBox "에뮬레이터 초기화 실패 (코드: " & lRes & ")", vbCritical, "오류"
            Exit Sub
    End Select
    
    ' UI 상태 전환
    m_bRunning = True
    cmdStop.Enabled = True
    cmdSelectBios.Enabled = False
    cmdSelectRom.Caption = "디스크 교체 (Swap CD)"
    
    If m_sRomPath <> "" Then
        Me.Caption = "PSXe 실행 중 - " & GetFileName(m_sRomPath)
    Else
        Me.Caption = "PSXe 실행 중 - 내장 BIOS 부팅"
    End If
    
    Dim frameCounter As Long
    
    ' === 메인 에뮬레이션 루프 ===
    Do While m_bRunning
        If PSXE_RunFrame() = 0 Then
            m_bRunning = False
            Exit Do
        End If
        
        ' 매번 DoEvents를 부르지 않고 3프레임에 한 번씩만 호출하여 속도 극대화
        frameCounter = frameCounter + 1
        If frameCounter Mod 3 = 0 Then
            DoEvents
        End If
    Loop
    
    ' 루프 탈출 시 정리
    PSXE_Shutdown
    
    cmdStop.Enabled = False
    cmdSelectBios.Enabled = True
    cmdSelectRom.Caption = "게임(CD/EXE) 열기 및 시작"
    Me.Caption = "PSXe VB6 Frontend (정지됨)"
    picScreen.Cls
End Sub

' 3. 정지 버튼
Private Sub cmdStop_Click()
    m_bRunning = False
End Sub

' =============================================================================
' 키보드 입력 처리 (방향키: 이동 / X: X버튼 / D: O버튼 / A: ㅁ버튼 / W: △버튼 / Enter: Start)
' =============================================================================
Private Function MapKeyToMask(ByVal KeyCode As Integer) As Long
    Select Case KeyCode
        Case vbKeyX:      MapKeyToMask = PSXI_SW_SDA_CROSS     ' X 버튼
        Case vbKeyD:      MapKeyToMask = PSXI_SW_SDA_CIRCLE    ' O 버튼
        Case vbKeyA:      MapKeyToMask = PSXI_SW_SDA_SQUARE    ' ㅁ 버튼
        Case vbKeyW:      MapKeyToMask = PSXI_SW_SDA_TRIANGLE  ' △ 버튼
        Case vbKeyReturn: MapKeyToMask = PSXI_SW_SDA_START     ' START
        Case vbKeyS:      MapKeyToMask = PSXI_SW_SDA_SELECT    ' SELECT
        Case vbKeyUp:     MapKeyToMask = PSXI_SW_SDA_PAD_UP
        Case vbKeyDown:   MapKeyToMask = PSXI_SW_SDA_PAD_DOWN
        Case vbKeyLeft:   MapKeyToMask = PSXI_SW_SDA_PAD_LEFT
        Case vbKeyRight:  MapKeyToMask = PSXI_SW_SDA_PAD_RIGHT
        Case vbKeyQ:      MapKeyToMask = PSXI_SW_SDA_L1
        Case vbKeyE:      MapKeyToMask = PSXI_SW_SDA_R1
        Case vbKey1:      MapKeyToMask = PSXI_SW_SDA_L2
        Case vbKey3:      MapKeyToMask = PSXI_SW_SDA_R2
        Case Else:        MapKeyToMask = 0
    End Select
End Function

Private Sub Form_KeyDown(KeyCode As Integer, Shift As Integer)
    If Not m_bRunning Then Exit Sub
    
    ' 기능 단축키 (F1: VRAM 전체 보기, F3: 세로모드, F4: 바이리니어 필터, F5: 소프트 리셋)
    Select Case KeyCode
        Case vbKeyF1: PSXE_Command 1: KeyCode = 0: Exit Sub
        Case vbKeyF3: PSXE_Command 3: KeyCode = 0: Exit Sub
        Case vbKeyF4: PSXE_Command 4: KeyCode = 0: Exit Sub
        Case vbKeyF5: PSXE_Command 5: KeyCode = 0: Exit Sub
    End Select
    
    Dim mask As Long
    mask = MapKeyToMask(KeyCode)
    If mask <> 0 Then
        PSXE_SetPadButton mask, 1
        KeyCode = 0 ' 방향키나 엔터키를 눌렀을 때 버튼 포커스가 이동하거나 딩~ 소리가 나는 것 방지
    End If
End Sub

Private Sub Form_KeyUp(KeyCode As Integer, Shift As Integer)
    If Not m_bRunning Then Exit Sub
    
    Dim mask As Long
    mask = MapKeyToMask(KeyCode)
    If mask <> 0 Then
        PSXE_SetPadButton mask, 0
        KeyCode = 0
    End If
End Sub

Private Sub Form_Unload(Cancel As Integer)
    If m_bRunning Then
        m_bRunning = False
        DoEvents
    End If
    PSXE_Shutdown
End Sub

' =============================================================================
' 유틸리티: Win32 API 파일 열기 대화상자 & 파일명 추출
' =============================================================================
Private Function ShowOpenDialog(ByVal sFilter As String, ByVal sTitle As String) As String
    Dim ofn As OPENFILENAME
    Dim sBuff As String
    Dim iPos As Long
    
    sBuff = String$(260, vbNullChar)
    
    With ofn
        .lStructSize = Len(ofn)
        .hwndOwner = Me.hWnd
        .hInstance = App.hInstance
        .lpstrFilter = Replace(sFilter, "|", vbNullChar) & vbNullChar & vbNullChar
        .lpstrFile = sBuff
        .nMaxFile = 260
        .lpstrInitialDir = App.Path
        .lpstrTitle = sTitle
        .Flags = OFN_FILEMUSTEXIST Or OFN_PATHMUSTEXIST Or OFN_HIDEREADONLY Or OFN_NOCHANGEDIR
    End With
    
    If GetOpenFileName(ofn) <> 0 Then
        iPos = InStr(ofn.lpstrFile, vbNullChar)
        If iPos > 0 Then
            ShowOpenDialog = Left$(ofn.lpstrFile, iPos - 1)
        Else
            ShowOpenDialog = ofn.lpstrFile
        End If
    Else
        ShowOpenDialog = ""
    End If
End Function

Private Function GetFileName(ByVal sFullPath As String) As String
    Dim iPos As Long
    iPos = InStrRev(sFullPath, "\")
    If iPos > 0 Then
        GetFileName = Mid$(sFullPath, iPos + 1)
    Else
        GetFileName = sFullPath
    End If
End Function

Private Sub Timer1_Timer()
    If m_bRunning Then
        Dim fFps As Single
        Dim sTitle As String
        
        fFps = PSXE_GetFPS()
        
        If m_sRomPath <> "" Then
            sTitle = GetFileName(m_sRomPath)
        Else
            sTitle = "내장 BIOS 부팅"
        End If
        
        ' 폼 타이틀바에 게임 이름과 실시간 FPS 표시
        Me.Caption = "PSXe 실행 중 - " & sTitle & " (FPS: " & Format(fFps, "0.0") & ")"
    End If
End Sub

