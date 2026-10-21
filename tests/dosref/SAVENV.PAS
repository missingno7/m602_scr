unit SavEnv;
{ Executable stand-in for the M602 environment that the reconstructed
  SAVER.PAS (m602_recon/src) links against.  Test harness only: it lets the
  real Turbo Pascal 6.01 compiler and RTL run the reconstructed saver so the
  native port can be compared with it frame by frame.

  The identifiers mirror m602_recon/probes/saver/SAVENV.PAS.  Unlike that
  linkage harness, these bodies are functional:
    BoxLines, InitialDY, InitialDX  initialized data DS:539C/53D8/53DE
    ToDisplayText                   1126:0131 is the identity for every byte
                                    the saver emits (ASCII + B3 C4 DA BF C0 D9)
                                    under the default KeybCS setting
    TwoDigits                       transcription of 119C:0201
    Maximum, Minimum                signed LongInt helpers 012C/016A
    ClearScreen                     Drivers.ClearScreen: INT 10h AX=0600h BH=07
                                    over the whole screen = words 0720h
  The video buffer is ordinary heap memory instead of B800:0000. }
{$S-,R-,F+}
interface
type TFarAddress = record Offset, Segment: Word end;
const
  BoxLines: array[0..4] of string[11] = (
    #218#196#196#196#196#196#196#196#196#196#191,
    'Software602',
    #179'    o    '#179,
    #179'    O    '#179,
    #192#196#196#196#196#196#196#196#196#196#217);
  InitialDY: array[1..3] of Integer = (-1, 1, -1);
  InitialDX: array[1..3] of Integer = (-1, 0, 1);
  CheckModem: Boolean = False;
var
  ScreenWidth, ScreenHeight: Byte;
  ScreenBuffer: Pointer;
  ShiftState: Byte;
function ToDisplayText(S: string): string;
function TwoDigits(N, Width: Integer): string;
function Maximum(A,B: LongInt): LongInt;
function Minimum(A,B: LongInt): LongInt;
function ModemWake: Boolean;
procedure ClearScreen;
implementation
function ToDisplayText(S: string): string;
begin
  ToDisplayText := S
end;
function TwoDigits(N, Width: Integer): string;
var S: string;
begin
  S[0] := Chr(Width);
  while Width > 0 do begin
    S[Width] := Chr(N mod 10 + 48);
    N := N div 10;
    Dec(Width)
  end;
  TwoDigits := S
end;
function Maximum(A,B: LongInt): LongInt;
begin if A > B then Maximum := A else Maximum := B end;
function Minimum(A,B: LongInt): LongInt;
begin if A < B then Minimum := A else Minimum := B end;
function ModemWake: Boolean;
begin ModemWake := False end;
procedure ClearScreen;
var I: Word;
begin
  for I := 0 to Word(ScreenWidth) * ScreenHeight - 1 do
    MemW[Seg(ScreenBuffer^):Ofs(ScreenBuffer^) + I * 2] := $0720
end;
end.
