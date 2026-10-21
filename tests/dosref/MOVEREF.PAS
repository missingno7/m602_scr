program MoveRef;
{ Runs the reconstructed ScreenSaver (m602_recon/src/SAVER.PAS, compiled
  unchanged by Turbo Pascal 6.01) and prints one trace line per frame.
  Test harness only; see gen_reference.py.

  Usage: MOVEREF width frames stepy cycle hour minute second hundredths

  - DOS date/time (INT 21h AH=2Ah/2Ch, used by Randomize, GetDate and
    GetTime) is answered from FakeDate/FakeTime so runs are deterministic;
    every other INT 21h call is chained to DOS.
  - INT 28h advances the BIOS tick counter by one so Pause(100) returns
    after its two-tick wait without real-time delay.
  - The uninitialized ScreenSaver locals StepY and Cycle are preset by
    Prefill, a procedure with the same caller and frame base that leaves
    the requested values in those stack slots.
  - GetEvent reads ScreenSaver's locals through the caller's BP. }
{$S-,R-,F+}
uses Dos, Objects, Drivers, Views, Saver, SavEnv;

const
  { BP-relative offsets of ScreenSaver's locals in the TP 6.01 build. }
  OfsNextY = -$06; OfsNextX = -$0C; OfsDY = -$1E; OfsDX = -$24;
  OfsStepY = -$2F; OfsCycle = -$3E;
  FrameSize = $60;

type
  PProbe = ^TProbe;
  TProbe = object(TView)
    procedure GetEvent(var Event: TEvent); virtual;
  end;

var
  FakeCX, FakeDX, FakeYear, FakeMD: Word;
  FakeDow: Byte;
  Width, Frames, InitStepY, InitCycle, Calls: Integer;
  Probe: TProbe;
  Bounds: TRect;
  StartupEvents: Word;
  Old21, Old28: Pointer;
  Code: Integer;

procedure Int21Hook; assembler;
asm
  cmp ah,$2C
  je @time
  cmp ah,$2A
  je @date
  db $EA          { jmp far: target patched with the previous vector }
  dw 0, 0
@time:
  mov cx,FakeCX
  mov dx,FakeDX
  iret
@date:
  mov cx,FakeYear
  mov dx,FakeMD
  mov al,FakeDow
  iret
end;

procedure Int28Hook; interrupt;
begin
  Inc(MemL[0:$46C])
end;

function Hex(L: LongInt; Digits: Integer): string;
const D: array[0..15] of Char = '0123456789ABCDEF';
var S: string; I: Integer;
begin
  S[0] := Chr(Digits);
  for I := Digits downto 1 do begin
    S[I] := D[L and 15];
    L := L shr 4
  end;
  Hex := S
end;

procedure SetFakeClock(Frame: Integer);
var Minute, Hour, Day, Month: Word;
begin
  { Varies every field the boxes display; mirrored by the C++ test. }
  Minute := (Frame div 7) mod 60;
  Hour := (Frame div 420 + 22) mod 24;
  Day := (Frame div 97) mod 31 + 1;
  Month := (Frame div 1000) mod 12 + 1;
  FakeCX := Hour shl 8 + Minute;
  FakeMD := Month shl 8 + Day
end;

procedure TProbe.GetEvent(var Event: TEvent);
var
  Frame: Word;
  I: Integer;
  Hash: LongInt;
  W: Word;
begin
  asm
    mov ax,[bp]
    mov Frame,ax
  end;
  Inc(Calls);
  Event.What := evNothing;
  if Calls <= 10 then Exit;
  Write(Calls - 10, ' ', Hex(RandSeed, 8));
  for I := 1 to 3 do Write(' ', MemW[SSeg:Frame + OfsNextX + I * 2 - 2]);
  for I := 1 to 3 do Write(' ', MemW[SSeg:Frame + OfsNextY + I * 2 - 2]);
  for I := 1 to 3 do Write(' ', Integer(MemW[SSeg:Frame + OfsDX + I * 2 - 2]));
  for I := 1 to 3 do Write(' ', Integer(MemW[SSeg:Frame + OfsDY + I * 2 - 2]));
  Write(' ', MemW[SSeg:Frame + OfsCycle], ' ', Mem[SSeg:Frame + OfsStepY]);
  Hash := 0;
  for I := 0 to Width * 25 - 1 do begin
    W := MemW[Seg(ScreenBuffer^):Ofs(ScreenBuffer^) + I * 2];
    Hash := Hash * 31 + W
  end;
  Writeln(' ', Hex(Hash, 8));
  SetFakeClock(Calls - 10);
  if Calls - 10 >= Frames then Event.What := evKeyDown
end;

procedure Prefill(Application: PView; var StartupEvents: Word);
var Area: array[1..FrameSize] of Byte;
begin
  FillChar(Area, SizeOf(Area), 0);
  Area[FrameSize + 1 + OfsStepY] := InitStepY;
  Area[FrameSize + 1 + OfsCycle] := Lo(InitCycle);
  Area[FrameSize + 2 + OfsCycle] := Hi(InitCycle)
end;

procedure Run;
begin
  Prefill(@Probe, StartupEvents);
  ScreenSaver(@Probe, StartupEvents)
end;

function Arg(N: Integer): Integer;
var V: Integer;
begin
  Val(ParamStr(N), V, Code);
  Arg := V
end;

begin
  Width := Arg(1); Frames := Arg(2); InitStepY := Arg(3); InitCycle := Arg(4);
  FakeCX := Arg(5) shl 8 + Arg(6);
  FakeDX := Arg(7) shl 8 + Arg(8);
  FakeYear := 1992; FakeMD := $0A15; FakeDow := 3;
  GetMem(ScreenBuffer, 32768);
  ScreenWidth := Width; ScreenHeight := 25; ShiftState := 0;
  Bounds.Assign(0, 0, 1, 1);
  Probe.Init(Bounds);
  GetIntVec($21, Old21);
  MemL[Seg(Int21Hook):Ofs(Int21Hook) + 11] := LongInt(Old21);
  GetIntVec($28, Old28);
  SetIntVec($28, @Int28Hook);
  SetIntVec($21, @Int21Hook);
  Run;
  SetIntVec($21, Old21);
  SetIntVec($28, Old28)
end.
