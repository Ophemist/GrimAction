using System.ComponentModel;
using System.Diagnostics;
using System.Reflection.PortableExecutable;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;

return InjectorProgram.Run(args);

internal static class InjectorProgram
{
    private const uint ProcessCreateThread=2, ProcessVmOperation=8, ProcessVmRead=0x10, ProcessVmWrite=0x20, ProcessQueryLimitedInformation=0x1000;
    private const uint MemCommit=0x1000, MemReserve=0x2000, MemRelease=0x8000, PageReadWrite=4;
    private const uint WaitObject0=0, WaitFailed=0xffffffff, LoggingActive=3, RequestAbi=2;
    // A timeout is unrecoverable: it retains remote storage and prohibits retry until the game exits.
    // These are deliberately generous, because remote initialization SHA-256s the executable and both
    // game modules before it returns, and a cold disk must not be misreported as a lost thread.
    private const uint LoadTimeoutMs=60000, InitializeTimeoutMs=120000;
    private const int PathChars=32768, StatusSize=128, RequestSize=131216, ResultOffset=131080, CompletedOffset=131208;
    private const int StopRequestSize=152, StopResultOffset=16, StopCompletedOffset=144;
    private const int FaultRequestSize=168, FaultResultOffset=16, FaultControlBeforeOffset=144, FaultRestoreBeforeOffset=152, FaultCompletedOffset=160;
    private const uint StoppedResident=5, ShutdownPending=6, WriterComplete=4;
    // The runtime's own writer join is bounded; the remote wait must outlast it so a slow flush is
    // reported as shutdown-pending, which is recoverable by exiting normally, rather than as an
    // indeterminate lost thread, which is not.
    private const uint StopJoinTimeoutMs=10000, StopTimeoutMs=60000;

    [UnmanagedFunctionPointer(CallingConvention.Cdecl, CharSet=CharSet.Unicode)]
    private delegate uint ValidateKnownFiles([MarshalAs(UnmanagedType.LPWStr)] string root);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl, CharSet=CharSet.Unicode)]
    private delegate uint CheckConfigFile([MarshalAs(UnmanagedType.LPWStr)] string path, nint errorText, uint capacity);

    public static int Run(string[] args)
    {
        if (args.Length == 1 && args[0] == "--self-test") return SelfTest();
        if (args.Length == 3 && args[0] == "--check-config") return CheckConfig(args[1], args[2]);
        if (args.Length == 3 && args[0] == "--stop") return RequestStop(args[1], args[2], 0);
        if (args.Length == 3 && args[0] == "--gate1-stop") return RequestStop(args[1], args[2], 1);
        if (args.Length == 3 && args[0] == "--gate2-stop") return RequestStop(args[1], args[2], 2);
        if (args.Length == 3 && args[0] == "--collision-stop") return RequestStop(args[1], args[2], 3);
        if (args.Length == 3 && args[0] == "--gate1-fault") return RequestProfileFault(args[1], args[2], 1);
        if (args.Length == 3 && args[0] == "--gate2-fault") return RequestProfileFault(args[1], args[2], 2);
        var argumentShape = RunArgumentShape(args);
        if (argumentShape < 0) return Fail("Supply [--gate1|--gate2|--collision] followed by the Grim Dawn root, runtime DLL, configuration, and output log paths.", 2);
        var gate = argumentShape;
        var offset = gate > 0 ? 1 : 0;
        if (!Environment.Is64BitProcess) return Fail("The injector must run as x64.", 2);
        var root=Path.GetFullPath(args[offset]); var runtime=Path.GetFullPath(args[offset+1]); var config=Path.GetFullPath(args[offset+2]); var log=Path.GetFullPath(args[offset+3]);
        var exe=Path.Combine(root,"x64","Grim Dawn.exe"); var reshade=Path.Combine(root,"x64","dxgi.dll");
        var addon=Path.Combine(root,"x64","ReshadeEffectShaderToggler.addon64");
        if (!File.Exists(runtime)||!File.Exists(config)||!File.Exists(exe)) return Fail("A required runtime, configuration, or game file does not exist.",3);
        // ReShade is optional. When its DLL or addon is installed, both are hashed before and after and must stay loaded and
        // unchanged; players without ReShade are not required to have either.
        var guarded=new List<string>(); if(File.Exists(reshade))guarded.Add(reshade); if(File.Exists(addon))guarded.Add(addon);
        Directory.CreateDirectory(Path.GetDirectoryName(log)!);
        nint localRuntime=0;
        try
        {
            var runtimeHash=Hash(runtime); var guardedHashes=new List<byte[]>(); foreach(var file in guarded)guardedHashes.Add(Hash(file));
            localRuntime=NativeLibrary.Load(runtime);
            var validate=Marshal.GetDelegateForFunctionPointer<ValidateKnownFiles>(NativeLibrary.GetExport(localRuntime,"GdTpcValidateKnownFiles"));
            if(validate(root)!=1) return Fail("The installed Grim Dawn files are not a supported build.",4);
            var initRva=ValidateExecutableRva(runtime,localRuntime,NativeLibrary.GetExport(localRuntime,"GdTpcInitializeRemote"));
            using var game=SelectTargetProcess(exe);
            if(game is null) return Fail("A supported Grim Dawn x64 process is not running.",5);
            if(FindRemoteModule(game,runtime)!=0) return Fail("The runtime is already resident; reinjection is prohibited.",6);
            var guardedBefore=new List<nint>(); foreach(var file in guarded)guardedBefore.Add(FindRemoteModule(game,file));
            using var process=OpenProcess(ProcessCreateThread|ProcessVmOperation|ProcessVmRead|ProcessVmWrite|ProcessQueryLimitedInformation,false,game.Id);
            if(process.IsInvalid) return Fail($"Could not open Grim Dawn (Win32 {Marshal.GetLastWin32Error()}).",7);

            var remoteLoadLibrary=ResolveRemoteFunction(game,"kernel32.dll","LoadLibraryW");
            var pathMemory=RemoteAllocation.FromUnicode(process,runtime);
            if(pathMemory is null) return Fail("Could not stage the runtime path.",9);
            var load=RunRemote(process,remoteLoadLibrary,pathMemory.Address,LoadTimeoutMs);
            if(load.State==RemoteCallState.NotStarted){pathMemory.Dispose();return Fail(load.Message,10);}
            if(load.State==RemoteCallState.Unknown){pathMemory.Retain();return Fail("INDETERMINATE load; storage retained and retry/unload prohibited until process exit. "+load.Message,21);}
            pathMemory.Dispose();
            game.Refresh(); var remoteRuntime=FindRemoteModule(game,runtime);
            if(remoteRuntime==0) return Fail("Load completed but exact-path module enumeration did not confirm the runtime. A module may be resident and inert; exit the game normally before any further attempt.",11);
            if(!CryptographicOperations.FixedTimeEquals(runtimeHash,Hash(runtime))) return Fail("Runtime image changed during load; no initialization attempted. The loaded runtime is resident and inert; exit the game normally before any further attempt.",11);

            var requestMemory=RemoteAllocation.FromBytes(process,BuildRequest(log,config));
            if(requestMemory is null) return Fail("Could not stage the versioned initialization request. The runtime is resident and inert; exit the game normally before any further attempt.",12);
            var initialized=RunRemote(process,AddRva(remoteRuntime,initRva),requestMemory.Address,InitializeTimeoutMs);
            if(initialized.State==RemoteCallState.NotStarted){requestMemory.Dispose();return Fail(initialized.Message,13);}
            if(initialized.State==RemoteCallState.Unknown){requestMemory.Retain();return Fail("INDETERMINATE initialization; storage retained and retry/unload prohibited until process exit. "+initialized.Message,22);}
            var returned=requestMemory.Read(RequestSize); requestMemory.Dispose();
            var completed=BitConverter.ToUInt32(returned,CompletedOffset); var abi=BitConverter.ToUInt32(returned,ResultOffset);
            var size=BitConverter.ToUInt32(returned,ResultOffset+4); var phase=BitConverter.ToUInt32(returned,ResultOffset+8);
            var hooks=BitConverter.ToUInt32(returned,ResultOffset+12); var writes=BitConverter.ToUInt32(returned,ResultOffset+16);
            var writerHealth=BitConverter.ToUInt32(returned,ResultOffset+20); var configLoaded=BitConverter.ToUInt32(returned,ResultOffset+28);
            var controlWrites=BitConverter.ToUInt64(returned,ResultOffset+64); var restoreWrites=BitConverter.ToUInt64(returned,ResultOffset+72);
            var collisionEnabled=BitConverter.ToUInt32(returned,ResultOffset+80);
            var expectedWrites=gate > 0 ? 1U : 0U;
            var expectedCollision=gate==3 ? 1U : 0U;
            if(completed!=1||abi!=RequestAbi||size!=StatusSize||phase!=LoggingActive||hooks!=1||writes!=expectedWrites||
               writerHealth!=1||configLoaded!=1||controlWrites!=0||restoreWrites!=0||collisionEnabled!=expectedCollision)
                return Fail($"Invalid/inactive status (completed={completed}, abi={abi}, size={size}, phase={phase}, hooks={hooks}, writes={writes}, writer_health={writerHealth}, config_loaded={configLoaded}, control_writes={controlWrites}, restore_writes={restoreWrites}, collision_enabled={collisionEnabled}). Do not retry or unload; exit the game normally.",13);
            game.Refresh();
            for(var index=0;index<guarded.Count;index++)
                if(FindRemoteModule(game,guarded[index])!=guardedBefore[index]||!CryptographicOperations.FixedTimeEquals(guardedHashes[index],Hash(guarded[index])))
                    return Fail("ReShade identity changed. Runtime may be active; exit normally and do not retry.",15);
            Console.WriteLine($"PASS: Gate {gate} hook active; hooks=1; config_loaded=1; writer_health=healthy; game_state_writes_enabled={expectedWrites}; control_writes=0; restore_writes=0; output={log}"); return 0;
        }
        catch(Exception e) when(e is InvalidOperationException or IOException or UnauthorizedAccessException or Win32Exception or BadImageFormatException or ArgumentException or NotSupportedException or OverflowException){return Fail(e.Message,20);}
        finally{if(localRuntime!=0)NativeLibrary.Free(localRuntime);}
    }

    // Checks a settings file with the runtime's own strict parser, loading the runtime DLL into this process only (never the
    // game). Used by the player launcher before injecting, so an invalid edit is reported instead of leaving an inert runtime.
    private static int CheckConfig(string runtimeArg, string configArg)
    {
        if (!Environment.Is64BitProcess) return Fail("The injector must run as x64.", 2);
        var runtime=Path.GetFullPath(runtimeArg); var config=Path.GetFullPath(configArg);
        if(!File.Exists(runtime)||!File.Exists(config)) return Fail("The runtime or settings file does not exist.",3);
        nint module=0; nint buffer=0;
        try
        {
            module=NativeLibrary.Load(runtime);
            var check=Marshal.GetDelegateForFunctionPointer<CheckConfigFile>(NativeLibrary.GetExport(module,"GdTpcCheckConfigFile"));
            const int capacity=1024;
            buffer=Marshal.AllocHGlobal(capacity*sizeof(char));
            Marshal.WriteInt16(buffer,0);
            var valid=check(config,buffer,capacity);
            var reason=Marshal.PtrToStringUni(buffer)??string.Empty;
            if(valid==1){Console.WriteLine("PASS: settings are valid: "+config);return 0;}
            return Fail("Settings rejected: "+(reason.Length>0?reason:"unknown reason"),23);
        }
        catch(Exception e) when(e is InvalidOperationException or IOException or UnauthorizedAccessException or DllNotFoundException or EntryPointNotFoundException or BadImageFormatException or ArgumentException){return Fail(e.Message,20);}
        finally{if(buffer!=0)Marshal.FreeHGlobal(buffer);if(module!=0)NativeLibrary.Free(module);}
    }

    // Requests the runtime's logical stop in a target where it is already resident. This never
    // detaches a hook, never calls FreeLibrary, and never terminates a thread: a logical stop only
    // ends observation and joins the writer. A stop whose completion cannot be confirmed is
    // INDETERMINATE, keeps its remote storage, and prohibits retry until the game exits normally.
    private static int RequestStop(string rootArg, string runtimeArg, int gate)
    {
        if (!Environment.Is64BitProcess) return Fail("The injector must run as x64.", 2);
        var root=Path.GetFullPath(rootArg); var runtime=Path.GetFullPath(runtimeArg);
        var exe=Path.Combine(root,"x64","Grim Dawn.exe");
        if(!File.Exists(runtime)||!File.Exists(exe)) return Fail("A required runtime or game file does not exist.",3);
        nint localRuntime=0;
        try
        {
            localRuntime=NativeLibrary.Load(runtime);
            var stopRva=ValidateExecutableRva(runtime,localRuntime,NativeLibrary.GetExport(localRuntime,"GdTpcRequestLogicalStopRemote"));
            using var game=SelectTargetProcess(exe);
            if(game is null) return Fail("A supported Grim Dawn x64 process is not running.",5);
            var remoteRuntime=FindRemoteModule(game,runtime);
            if(remoteRuntime==0) return Fail("This runtime is not resident in the target; there is nothing to stop.",16);
            using var process=OpenProcess(ProcessCreateThread|ProcessVmOperation|ProcessVmRead|ProcessVmWrite|ProcessQueryLimitedInformation,false,game.Id);
            if(process.IsInvalid) return Fail($"Could not open Grim Dawn (Win32 {Marshal.GetLastWin32Error()}).",7);

            var requestMemory=RemoteAllocation.FromBytes(process,BuildStopRequest(StopJoinTimeoutMs));
            if(requestMemory is null) return Fail("Could not stage the versioned stop request.",12);
            var stopped=RunRemote(process,AddRva(remoteRuntime,stopRva),requestMemory.Address,StopTimeoutMs);
            if(stopped.State==RemoteCallState.NotStarted){requestMemory.Dispose();return Fail(stopped.Message,17);}
            if(stopped.State==RemoteCallState.Unknown){requestMemory.Retain();return Fail("INDETERMINATE stop; storage retained and retry/unload prohibited until process exit. Exit the game normally. "+stopped.Message,23);}
            var returned=requestMemory.Read(StopRequestSize); requestMemory.Dispose();
            var completed=BitConverter.ToUInt32(returned,StopCompletedOffset);
            var abi=BitConverter.ToUInt32(returned,StopResultOffset); var size=BitConverter.ToUInt32(returned,StopResultOffset+4);
            var phase=BitConverter.ToUInt32(returned,StopResultOffset+8); var hooks=BitConverter.ToUInt32(returned,StopResultOffset+12);
            var writes=BitConverter.ToUInt32(returned,StopResultOffset+16); var writerHealth=BitConverter.ToUInt32(returned,StopResultOffset+20);
            var restoreState=BitConverter.ToUInt32(returned,StopResultOffset+24);
            var configLoaded=BitConverter.ToUInt32(returned,StopResultOffset+28);
            var controlWrites=BitConverter.ToUInt64(returned,StopResultOffset+64); var restoreWrites=BitConverter.ToUInt64(returned,StopResultOffset+72);
            if(completed!=1||abi!=RequestAbi||size!=StatusSize)
                return Fail($"The stop result is not a valid v2 status (completed={completed}, abi={abi}, size={size}); do not retry.",18);
            if(writes!=0||(gate==0&&(controlWrites!=0||restoreWrites!=0)))
                return Fail($"The runtime reports an invalid post-stop write state (enabled={writes}, control={controlWrites}, restore={restoreWrites}); exit the game normally.",18);
            if(gate>0&&restoreState is not (0 or 2))
                return Fail($"Gate {gate} restoration is not clean or verified (restore_state={restoreState}); exit the game normally.",18);
            if(phase==ShutdownPending)
                return Fail($"Writer completion was NOT confirmed; the runtime retains its writer resources (writer_health={writerHealth}). Do not retry or unload; exit the game normally.",19);
            if(phase!=StoppedResident)
                return Fail($"Logical stop did not reach stopped-resident (phase={phase}, hooks={hooks}, writer_health={writerHealth}).",18);
            if(writerHealth!=WriterComplete)
                return Fail($"Stopped-resident was reported but the writer is not complete (writer_health={writerHealth}).",19);
            if(hooks!=1||configLoaded!=1)
                return Fail($"Stopped-resident status lost required state (hooks={hooks}, config_loaded={configLoaded}).",18);
            Console.WriteLine($"PASS: Gate {gate} logical stop confirmed; phase=stopped_resident; writer complete; hooks=1 (original-only, still pinned); config_loaded=1; game_state_writes_enabled=0; restore_state={restoreState}; control_writes={controlWrites}; restore_writes={restoreWrites}");
            return 0;
        }
        catch(Exception e) when(e is InvalidOperationException or IOException or UnauthorizedAccessException or Win32Exception or BadImageFormatException or ArgumentException or NotSupportedException or OverflowException){return Fail(e.Message,20);}
        finally{if(localRuntime!=0)NativeLibrary.Free(localRuntime);}
    }

    private static int RequestProfileFault(string rootArg,string runtimeArg,int gate)
    {
        if(!Environment.Is64BitProcess)return Fail("The injector must run as x64.",2);
        var root=Path.GetFullPath(rootArg);var runtime=Path.GetFullPath(runtimeArg);var exe=Path.Combine(root,"x64","Grim Dawn.exe");
        if(!File.Exists(runtime)||!File.Exists(exe))return Fail("A required runtime or game file does not exist.",3);
        nint localRuntime=0;
        try
        {
            localRuntime=NativeLibrary.Load(runtime);
            var faultRva=ValidateExecutableRva(runtime,localRuntime,NativeLibrary.GetExport(localRuntime,"GdTpcRequestGate1RecoverableFaultRemote"));
            using var game=SelectTargetProcess(exe);if(game is null)return Fail("A supported Grim Dawn x64 process is not running.",5);
            var remoteRuntime=FindRemoteModule(game,runtime);if(remoteRuntime==0)return Fail($"This Gate {gate} runtime is not resident in the target.",16);
            using var process=OpenProcess(ProcessCreateThread|ProcessVmOperation|ProcessVmRead|ProcessVmWrite|ProcessQueryLimitedInformation,false,game.Id);
            if(process.IsInvalid)return Fail($"Could not open Grim Dawn (Win32 {Marshal.GetLastWin32Error()}).",7);
            var requestMemory=RemoteAllocation.FromBytes(process,BuildFaultRequest(StopJoinTimeoutMs));
            if(requestMemory is null)return Fail($"Could not stage the Gate {gate} fault request.",12);
            var call=RunRemote(process,AddRva(remoteRuntime,faultRva),requestMemory.Address,StopTimeoutMs);
            if(call.State==RemoteCallState.NotStarted){requestMemory.Dispose();return Fail(call.Message,17);}
            if(call.State==RemoteCallState.Unknown){requestMemory.Retain();return Fail("INDETERMINATE fault request; storage retained. Do not retry or unload; exit normally. "+call.Message,23);}
            var returned=requestMemory.Read(FaultRequestSize);requestMemory.Dispose();
            var completed=BitConverter.ToUInt32(returned,FaultCompletedOffset);var abi=BitConverter.ToUInt32(returned,FaultResultOffset);
            var size=BitConverter.ToUInt32(returned,FaultResultOffset+4);var phase=BitConverter.ToUInt32(returned,FaultResultOffset+8);
            var hooks=BitConverter.ToUInt32(returned,FaultResultOffset+12);var writes=BitConverter.ToUInt32(returned,FaultResultOffset+16);
            var writer=BitConverter.ToUInt32(returned,FaultResultOffset+20);var restore=BitConverter.ToUInt32(returned,FaultResultOffset+24);
            var control=BitConverter.ToUInt64(returned,FaultResultOffset+64);var restored=BitConverter.ToUInt64(returned,FaultResultOffset+72);
            var controlBefore=BitConverter.ToUInt64(returned,FaultControlBeforeOffset);var restoreBefore=BitConverter.ToUInt64(returned,FaultRestoreBeforeOffset);
            var expectedRestoreDelta=gate>=2 ? 11UL : 10UL;
            if(completed!=1||abi!=RequestAbi||size!=StatusSize||phase!=LoggingActive||hooks!=1||writes!=0||writer!=1||restore!=2||
               control!=controlBefore+4||restored!=restoreBefore+expectedRestoreDelta)
                return Fail($"Recoverable-fault proof failed (completed={completed}, phase={phase}, hooks={hooks}, writes={writes}, writer={writer}, restore_state={restore}, control_delta={control-controlBefore}, restore_delta={restored-restoreBefore}). Exit normally; do not retry.",18);
            Console.WriteLine($"PASS: Gate {gate} controlled write refusal restored all {expectedRestoreDelta} native fields; restore_state=verified; control_writes_delta=4; restore_writes_delta={expectedRestoreDelta}; further control disabled.");return 0;
        }
        catch(Exception e) when(e is InvalidOperationException or IOException or UnauthorizedAccessException or Win32Exception or BadImageFormatException or ArgumentException or NotSupportedException or OverflowException){return Fail(e.Message,20);}
        finally{if(localRuntime!=0)NativeLibrary.Free(localRuntime);}
    }

    private static byte[] BuildStopRequest(uint joinTimeoutMs)
    {
        var b=new byte[StopRequestSize]; BitConverter.GetBytes(RequestAbi).CopyTo(b,0);
        BitConverter.GetBytes((uint)StopRequestSize).CopyTo(b,4); BitConverter.GetBytes(joinTimeoutMs).CopyTo(b,8); return b;
    }
    private static byte[] BuildFaultRequest(uint timeoutMs)
    {
        var b=new byte[FaultRequestSize];BitConverter.GetBytes(RequestAbi).CopyTo(b,0);
        BitConverter.GetBytes((uint)FaultRequestSize).CopyTo(b,4);BitConverter.GetBytes(timeoutMs).CopyTo(b,8);return b;
    }

    private static Process? SelectTargetProcess(string exe)
    {
        Process? selected=null;
        foreach(var candidate in Process.GetProcessesByName("Grim Dawn"))
        {
            if(selected is null&&MatchesExecutable(candidate,exe)) selected=candidate; else candidate.Dispose();
        }
        return selected;
    }

    private static int SelfTest()
    {
        try
        {
            var r=BuildRequest(@"C:\logs\gate0.csv",@"C:\config\runtime.ini");
            Require(r.Length==RequestSize&&BitConverter.ToUInt32(r,0)==RequestAbi&&BitConverter.ToUInt32(r,4)==RequestSize,"request ABI/layout");
            Require(BitConverter.ToUInt32(r,CompletedOffset)==0,"request completion starts clear");
            Require(AddRva(new nint(0x0000018000000000),new nint(0x123456))==new nint(0x0000018000123456),"full-width address arithmetic");
            var localKernel32=GetModuleHandleW("kernel32.dll");
            Require(ResolveRemoteFunction(Process.GetCurrentProcess(),"kernel32.dll","LoadLibraryW")==NativeLibrary.GetExport(localKernel32,"LoadLibraryW"),
                "forwarded-export owner resolution");
            Require(ClassifyWait(WaitObject0,true)==RemoteCallState.Completed,"completed wait");
            Require(ClassifyWait(258,true)==RemoteCallState.Unknown&&ClassifyWait(WaitFailed,true)==RemoteCallState.Unknown,"unknown completion");
            Require(ClassifyWait(WaitFailed,false)==RemoteCallState.NotStarted,"not-started state");

            // Storage ownership: only a not-started or confirmed-complete call may free remote memory.
            Require(!MayFreeRemoteStorage(RemoteCallState.Unknown)&&MayFreeRemoteStorage(RemoteCallState.NotStarted)&&
                MayFreeRemoteStorage(RemoteCallState.Completed),"remote storage ownership by call state");

            // Paths at or beyond the fixed request capacity must be refused, never truncated into
            // an unterminated fixed-size field the runtime would reject after the thread had run.
            Require(Throws(()=>BuildRequest(new string('a',PathChars),"C:/config/runtime.ini")),"oversized log path rejected");
            Require(Throws(()=>BuildRequest(@"C:\logs\gate0.csv",new string('a',PathChars))),"oversized config path rejected");
            var maximal=BuildRequest(new string('a',PathChars-1),new string('b',PathChars-1));
            Require(maximal.Length==RequestSize&&maximal[8+(PathChars*2)-2]==0&&maximal[8+(PathChars*2)-1]==0,"maximal log path stays terminated");
            Require(maximal[ResultOffset-2]==0&&maximal[ResultOffset-1]==0,"maximal config path stays terminated");

            // Address arithmetic must not silently wrap, and a non-executable RVA must be refused.
            Require(Throws(()=>AddRva(new nint(long.MaxValue),new nint(1))),"address overflow rejected");
            var self=Environment.ProcessPath!;
            var selfBase=GetModuleHandleW(null!);
            Require(Throws(()=>ValidateExecutableRva(self,selfBase,selfBase)),"image-header RVA is not executable");
            Require(Throws(()=>ValidateExecutableRva(self,selfBase,selfBase-1)),"negative RVA rejected");
            Require(Throws(()=>ResolveRemoteFunction(Process.GetCurrentProcess(),"kernel32.dll","GdTpcNoSuchExport")),
                "missing export rejected");

            // Stop request layout must match the native GdTpcRemoteStopRequest exactly.
            var stopRequest=BuildStopRequest(StopJoinTimeoutMs);
            Require(stopRequest.Length==StopRequestSize&&BitConverter.ToUInt32(stopRequest,0)==RequestAbi&&
                BitConverter.ToUInt32(stopRequest,4)==StopRequestSize&&BitConverter.ToUInt32(stopRequest,8)==StopJoinTimeoutMs,
                "stop request ABI/layout");
            Require(BitConverter.ToUInt32(stopRequest,StopCompletedOffset)==0,"stop request completion starts clear");
            Require(StopResultOffset+StatusSize<=StopCompletedOffset,"stop result does not overlap the completion marker");
            Require(StopTimeoutMs>StopJoinTimeoutMs,"the remote stop wait must outlast the runtime's own writer join");
            var faultRequest=BuildFaultRequest(StopJoinTimeoutMs);
            Require(faultRequest.Length==FaultRequestSize&&BitConverter.ToUInt32(faultRequest,4)==FaultRequestSize&&
                FaultResultOffset+StatusSize<=FaultControlBeforeOffset&&FaultRestoreBeforeOffset+8<=FaultCompletedOffset,
                "Gate 1 fault request ABI/layout");

            // Gate selection is explicit and cannot be inferred from a write-capable DLL name.
            Require(RunArgumentShape(new[]{"root","runtime","config","log"})==0,"Gate 0 argument shape");
            Require(RunArgumentShape(new[]{"--gate1","root","runtime","config","log"})==1,"Gate 1 argument shape");
            Require(RunArgumentShape(new[]{"--gate2","root","runtime","config","log"})==2,"Gate 2 argument shape");
            Require(RunArgumentShape(new[]{"--collision","root","runtime","config","log"})==3,"collision argument shape");
            Require(RunArgumentShape(new[]{"--gate1","root","runtime","config"})==-1,"incomplete Gate 1 arguments rejected");
            Require(StopArgumentShape(new[]{"--stop","root","runtime"})==0,"Gate 0 stop argument shape");
            Require(StopArgumentShape(new[]{"--gate1-stop","root","runtime"})==1,"Gate 1 stop argument shape");
            Require(StopArgumentShape(new[]{"--gate2-stop","root","runtime"})==2,"Gate 2 stop argument shape");
            Require(StopArgumentShape(new[]{"--collision-stop","root","runtime"})==3,"collision stop argument shape");

            Console.WriteLine("PASS: injector request ABI/capacity, full-width address arithmetic and overflow, executable-RVA "+
                "validation, forwarded-export owner resolution, and timeout/storage ownership states."); return 0;
        }catch(Exception e){return Fail("self-test: "+e.Message,1);}
    }

    private static int RunArgumentShape(string[] args) =>
        args.Length==5&&args[0]=="--gate1" ? 1 :
        args.Length==5&&args[0]=="--gate2" ? 2 :
        args.Length==5&&args[0]=="--collision" ? 3 :
        args.Length==4&&!args[0].StartsWith("--",StringComparison.Ordinal) ? 0 : -1;
    private static int StopArgumentShape(string[] args) => args.Length==3&&args[0]=="--stop" ? 0 :
        args.Length==3&&args[0]=="--gate1-stop" ? 1 :
        args.Length==3&&args[0]=="--gate2-stop" ? 2 :
        args.Length==3&&args[0]=="--collision-stop" ? 3 : -1;

    private static byte[] BuildRequest(string log,string config)
    {
        if(log.Length>=PathChars||config.Length>=PathChars)throw new InvalidOperationException("A request path is too long.");
        var b=new byte[RequestSize]; BitConverter.GetBytes(RequestAbi).CopyTo(b,0); BitConverter.GetBytes(RequestSize).CopyTo(b,4);
        Encoding.Unicode.GetBytes(log+'\0').CopyTo(b,8); Encoding.Unicode.GetBytes(config+'\0').CopyTo(b,8+PathChars*2); return b;
    }
    private static nint ResolveRemoteFunction(Process process,string requestedModule,string export)
    {
        var requested=GetModuleHandleW(requestedModule); if(requested==0)throw new InvalidOperationException($"Local {requestedModule} unavailable.");
        var address=NativeLibrary.GetExport(requested,export);
        if(VirtualQuery(address,out var memory,(nuint)Marshal.SizeOf<MEMORY_BASIC_INFORMATION>())==0||memory.AllocationBase==0)
            throw new InvalidOperationException($"Could not establish owner image for {export}.");
        var ownerPath=GetLocalModulePath(memory.AllocationBase); var rva=ValidateExecutableRva(ownerPath,memory.AllocationBase,address);
        var remoteOwner=FindRemoteModule(process,ownerPath); if(remoteOwner==0)throw new InvalidOperationException($"Target lacks exact owner image for {export}.");
        var remotePath=FindRemoteModulePath(process,remoteOwner)!;
        if(!CryptographicOperations.FixedTimeEquals(Hash(ownerPath),Hash(remotePath)))throw new InvalidOperationException($"Local/remote owner images differ for {export}.");
        return AddRva(remoteOwner,rva);
    }
    private static nint ValidateExecutableRva(string imagePath,nint imageBase,nint address)
    {
        var delta=address.ToInt64()-imageBase.ToInt64(); if(delta<0||delta>int.MaxValue)throw new InvalidOperationException("Export RVA outside bounds.");
        using var stream=File.OpenRead(imagePath); using var pe=new PEReader(stream);
        if(pe.PEHeaders.CoffHeader.Machine!=Machine.Amd64)throw new BadImageFormatException("Owner image is not x64.");
        var rva=(int)delta; var section=pe.PEHeaders.SectionHeaders.FirstOrDefault(s=>rva>=s.VirtualAddress&&rva<s.VirtualAddress+Math.Max(s.VirtualSize,s.SizeOfRawData));
        if(section.Name is null||(section.SectionCharacteristics&SectionCharacteristics.MemExecute)==0)throw new InvalidOperationException("Export RVA is not executable.");
        return new nint(rva);
    }
    private static nint AddRva(nint b,nint r)=>checked(new nint(b.ToInt64()+r.ToInt64()));
    private static byte[] Hash(string p){using var f=File.Open(p,FileMode.Open,FileAccess.Read,FileShare.Read);return SHA256.HashData(f);}
    private static bool MatchesExecutable(Process p,string expected){try{return string.Equals(p.MainModule?.FileName,expected,StringComparison.OrdinalIgnoreCase);}catch{return false;}}
    private static nint FindRemoteModule(Process p,string nameOrPath){var path=Path.IsPathFullyQualified(nameOrPath);foreach(ProcessModule m in p.Modules)if(path?string.Equals(Path.GetFullPath(m.FileName),Path.GetFullPath(nameOrPath),StringComparison.OrdinalIgnoreCase):string.Equals(m.ModuleName,nameOrPath,StringComparison.OrdinalIgnoreCase))return m.BaseAddress;return 0;}
    private static string? FindRemoteModulePath(Process p,nint address){foreach(ProcessModule m in p.Modules)if(m.BaseAddress==address)return m.FileName;return null;}
    private static string GetLocalModulePath(nint module){var b=new StringBuilder(PathChars);var n=GetModuleFileNameW(module,b,b.Capacity);if(n==0||n>=b.Capacity)throw new Win32Exception(Marshal.GetLastWin32Error());return Path.GetFullPath(b.ToString());}
    private static RemoteCallResult RunRemote(SafeProcessHandle process,nint start,nint arg,uint timeoutMs)
    {
        using var thread=CreateRemoteThread(process,0,0,start,arg,0,out _); if(thread.IsInvalid)return new(RemoteCallState.NotStarted,$"CreateRemoteThread failed (Win32 {Marshal.GetLastWin32Error()}).");
        var wait=WaitForSingleObject(thread,timeoutMs);var state=ClassifyWait(wait,true);if(state!=RemoteCallState.Completed)return new(state,wait==WaitFailed?$"wait failed (Win32 {Marshal.GetLastWin32Error()}).":$"wait returned {wait}.");
        if(!GetExitCodeThread(thread,out var exit))return new(RemoteCallState.Unknown,$"exit-code read failed (Win32 {Marshal.GetLastWin32Error()}).");return new(RemoteCallState.Completed,$"exit={exit}");
    }
    private static RemoteCallState ClassifyWait(uint wait,bool started)=>!started?RemoteCallState.NotStarted:wait==WaitObject0?RemoteCallState.Completed:RemoteCallState.Unknown;
    private static int Fail(string m,int c){Console.Error.WriteLine("Injector failed closed: "+m);return c;}
    private static void Require(bool c,string n){if(!c)throw new InvalidOperationException(n);}
    private static bool Throws(Action action){try{action();return false;}catch(Exception){return true;}}
    // Remote argument storage may be reclaimed only when no thread started or completion was confirmed.
    private static bool MayFreeRemoteStorage(RemoteCallState state)=>state is RemoteCallState.NotStarted or RemoteCallState.Completed;
    private enum RemoteCallState{NotStarted,Completed,Unknown}
    private readonly record struct RemoteCallResult(RemoteCallState State,string Message);

    private sealed class RemoteAllocation:IDisposable
    {
        private readonly SafeProcessHandle process;private bool retained;public nint Address{get;}
        private RemoteAllocation(SafeProcessHandle p,nint a){process=p;Address=a;}
        public static RemoteAllocation? FromUnicode(SafeProcessHandle p,string v)=>FromBytes(p,Encoding.Unicode.GetBytes(v+'\0'));
        public static RemoteAllocation? FromBytes(SafeProcessHandle p,byte[] b){var a=VirtualAllocEx(p,0,(nuint)b.Length,MemCommit|MemReserve,PageReadWrite);if(a==0)return null;if(!WriteProcessMemory(p,a,b,(nuint)b.Length,out var w)||w!=(nuint)b.Length){VirtualFreeEx(p,a,0,MemRelease);return null;}return new(p,a);}
        public byte[] Read(int size){var b=new byte[size];if(!ReadProcessMemory(process,Address,b,(nuint)size,out var n)||n!=(nuint)size)throw new Win32Exception(Marshal.GetLastWin32Error());return b;}
        public void Retain()=>retained=true;public void Dispose(){if(!retained&&Address!=0)VirtualFreeEx(process,Address,0,MemRelease);}
    }
    [StructLayout(LayoutKind.Sequential)]private struct MEMORY_BASIC_INFORMATION{public nint BaseAddress,AllocationBase;public uint AllocationProtect;public ushort PartitionId;public nuint RegionSize;public uint State,Protect,Type;}
    [DllImport("kernel32.dll",SetLastError=true)]private static extern SafeProcessHandle OpenProcess(uint access,bool inherit,int pid);
    [DllImport("kernel32.dll",SetLastError=true)]private static extern nint VirtualAllocEx(SafeProcessHandle p,nint a,nuint s,uint t,uint protect);
    [DllImport("kernel32.dll",SetLastError=true)]private static extern bool VirtualFreeEx(SafeProcessHandle p,nint a,nuint s,uint t);
    [DllImport("kernel32.dll",SetLastError=true)]private static extern bool WriteProcessMemory(SafeProcessHandle p,nint a,byte[] b,nuint s,out nuint n);
    [DllImport("kernel32.dll",SetLastError=true)]private static extern bool ReadProcessMemory(SafeProcessHandle p,nint a,byte[] b,nuint s,out nuint n);
    [DllImport("kernel32.dll",SetLastError=true)]private static extern SafeThreadHandle CreateRemoteThread(SafeProcessHandle p,nint attrs,nuint stack,nint start,nint arg,uint flags,out uint id);
    [DllImport("kernel32.dll",SetLastError=true)]private static extern uint WaitForSingleObject(SafeThreadHandle h,uint ms);
    [DllImport("kernel32.dll",SetLastError=true)]private static extern bool GetExitCodeThread(SafeThreadHandle h,out uint code);
    [DllImport("kernel32.dll",CharSet=CharSet.Unicode)]private static extern nint GetModuleHandleW(string n);
    [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)]private static extern int GetModuleFileNameW(nint m,StringBuilder p,int c);
    [DllImport("kernel32.dll")]private static extern nuint VirtualQuery(nint a,out MEMORY_BASIC_INFORMATION i,nuint l);
}
internal sealed class SafeProcessHandle:Microsoft.Win32.SafeHandles.SafeHandleZeroOrMinusOneIsInvalid{private SafeProcessHandle():base(true){}protected override bool ReleaseHandle()=>CloseHandle(handle);[DllImport("kernel32.dll")]private static extern bool CloseHandle(nint h);}
internal sealed class SafeThreadHandle:Microsoft.Win32.SafeHandles.SafeHandleZeroOrMinusOneIsInvalid{private SafeThreadHandle():base(true){}protected override bool ReleaseHandle()=>CloseHandle(handle);[DllImport("kernel32.dll")]private static extern bool CloseHandle(nint h);}
