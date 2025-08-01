# Contract PrecodeStubs

This contract provides support for examining [precode](../coreclr/botr/method-descriptor.md#precode): small fragments of code used to implement temporary entry points and an efficient wrapper for stubs.

## APIs of contract

```csharp
    // Gets a pointer to the MethodDesc for a given stub entrypoint
    TargetPointer GetMethodDescFromStubAddress(TargetCodePointer entryPoint);
```

## Version 1, 2, and 3

Data descriptors used:
| Data Descriptor Name | Field | Meaning |
| --- | --- | --- |
| PrecodeMachineDescriptor | OffsetOfPrecodeType | See `ReadPrecodeType` (Version 1 and 2 only) |
| PrecodeMachineDescriptor | ShiftOfPrecodeType | See `ReadPrecodeType`  (Version 1 and 2 only) |
| PrecodeMachineDescriptor | ReadWidthOfPrecodeType | See `ReadPrecodeType`  (Version 1 and 2 only) |
| PrecodeMachineDescriptor | StubCodePageSize | Size of a precode code page (in bytes) |
| PrecodeMachineDescriptor | CodePointerToInstrPointerMask | mask to apply to code pointers to get an address (see arm32 note)
| PrecodeMachineDescriptor | StubPrecodeType | precode sort byte for stub precodes |
| PrecodeMachineDescriptor | HasPInvokeImportPrecode | 1 if platform supports PInvoke precode stubs |
| PrecodeMachineDescriptor | PInvokeImportPrecodeType |  precode sort byte for PInvoke precode stubs, if supported |
| PrecodeMachineDescriptor | HasFixupPrecode | 1 if platform supports fixup precode stubs |
| PrecodeMachineDescriptor | FixupPrecodeType | precode sort byte for fixup precode stubs, if supported |
| PrecodeMachineDescriptor | ThisPointerRetBufPrecodeType | precode sort byte for this pointer ret buf precodes |
| PrecodeMachineDescriptor | FixupStubPrecodeSize | Byte size of `FixupBytes` and `FixupIgnoredBytes` (Version 3 only) |
| PrecodeMachineDescriptor | FixupBytes | Assembly code of a FixupStub (Version 3 only) |
| PrecodeMachineDescriptor | FixupIgnoredBytes | Bytes to ignore of when comparing `FixupBytes` to an actual block of memory in the target process. (Version 3 only) |
| PrecodeMachineDescriptor | StubPrecodeSize | Byte size of `StubBytes` and `StubIgnoredBytes` (Version 3 only) |
| PrecodeMachineDescriptor | StubBytes | Assembly code of a StubPrecode (Version 3 only) |
| PrecodeMachineDescriptor | StubIgnoredBytes | Bytes to ignore of when comparing `StubBytes` to an actual block of memory in the target process. (Version 3 only) |
| PrecodeMachineDescriptor | FixupCodeOffset | Offset of second entrypoint into a `FixupStub` (Present in data for Version 3 and above only.) |
| PrecodeMachineDescriptor | InterpreterPrecodeType | precode sort byte for the entrypoint into the interpreter (Version 3 only) |
| PrecodeMachineDescriptor | UMEntryPrecodeType | precode sort byte for the entrypoint into the UMEntry thunk (Version 3 only) |
| PrecodeMachineDescriptor | DynamicHelperPrecodeType | precode sort byte for the entrypoint into a dynamic helper (Version 3 only) |
| StubPrecodeData | MethodDesc | pointer to the MethodDesc associated with this stub precode (Version 1 only) |
| StubPrecodeData | SecretParam | pointer to the MethodDesc associated with this stub precode or a second stub data pointer for other types (Version 2 only) |
| StubPrecodeData | Type | precise sort of stub precode |
| FixupPrecodeData | MethodDesc | pointer to the MethodDesc associated with this fixup precode |
| ThisPtrRetBufPrecodeData | MethodDesc | pointer to the MethodDesc associated with the ThisPtrRetBufPrecode (Version 2 only) |

arm32 note: the `CodePointerToInstrPointerMask` is used to convert IP values that may include an arm Thumb bit (for example extracted from disassembling a call instruction or from a snapshot of the registers) into an address.  On other architectures applying the mask is a no-op.


Global variables used:
| Global Name | Type | Purpose |
| --- | --- | --- |
| PrecodeMachineDescriptor | pointer | address of the `PrecodeMachineDescriptor` data |

Contracts used:
| Contract Name |
| --- |
| `PlatformMetadata` |

### Determining the precode type (Version 3)
``` csharp
    private bool ReadBytesAndCompare(TargetPointer instrAddress, byte[] expectedBytePattern, byte[] bytesToIgnore)
    {
        byte[] localCopy = new byte[expectedBytePattern.Length];
        for (int i = 0; i < expectedBytePattern.Length; i++)
        {
            if (bytesToIgnore[i] == 0)
            {
                byte targetBytePattern = _target.Read<byte>(instrAddress + i);
                if (expectedBytePattern[i] != targetBytePattern)
                {
                    return false;
                }
            }
        }

        return true;
    }
    private KnownPrecodeType? TryGetKnownPrecodeType(TargetPointer instrAddress)
    {
        KnownPrecodeType? basicPrecodeType = default;
        if (ReadBytesAndCompare(instrAddress, MachineDescriptor.StubBytes, MachineDescriptor.StubIgnoredBytes))
        {
            // get the actual type from the StubPrecodeData
            Data.StubPrecodeData stubPrecodeData = GetStubPrecodeData(instrAddress);
            byte exactPrecodeType = stubPrecodeData.Type;
            if (exactPrecodeType == 0)
                return null;

            if (exactPrecodeType == MachineDescriptor.StubPrecodeType)
            {
                return KnownPrecodeType.Stub;
            }
            else if (MachineDescriptor.PInvokeImportPrecodeType is byte compareByte1 && compareByte1 == exactPrecodeType)
            {
                return KnownPrecodeType.PInvokeImport;
            }
            else if (MachineDescriptor.ThisPointerRetBufPrecodeType is byte compareByte2 && compareByte2 == exactPrecodeType)
            {
                return KnownPrecodeType.ThisPtrRetBuf;
            }
            else if (MachineDescriptor.UMEntryPrecodeType is byte compareByte3 && compareByte3 == exactPrecodeType)
            {
                return KnownPrecodeType.UMEntry;
            }
            else if (MachineDescriptor.InterpreterPrecodeType is byte compareByte4 && compareByte4 == exactPrecodeType)
            {
                return KnownPrecodeType.Interpreter;
            }
            else if (MachineDescriptor.DynamicHelperPrecodeType is byte compareByte5 && compareByte5 == exactPrecodeType)
            {
                return KnownPrecodeType.DynamicHelper;
            }
        }
        else if (ReadBytesAndCompare(instrAddress, MachineDescriptor.FixupBytes, MachineDescriptor.FixupIgnoredBytes))
        {
            return KnownPrecodeType.Fixup;
        }
        return null;
    }
```

### Determining the precode type (Version 1 and 2)

An initial approximation of the precode type relies on a particular pattern at a known offset from the precode entrypoint.
The precode type is expected to be encoded as an immediate. On some platforms the value is spread over multiple instruction bytes and may need to be right-shifted.

```csharp
    private byte ReadPrecodeType(TargetPointer instrPointer)
    {
        if (MachineDescriptor.ReadWidthOfPrecodeType == 1)
        {
            byte precodeType = _target.Read<byte>(instrPointer + MachineDescriptor.OffsetOfPrecodeType);
            return (byte)(precodeType >> MachineDescriptor.ShiftOfPrecodeType);
        }
        else if (MachineDescriptor.ReadWidthOfPrecodeType == 2)
        {
            ushort precodeType = _target.Read<ushort>(instrPointer + MachineDescriptor.OffsetOfPrecodeType);
            return (byte)(precodeType >> MachineDescriptor.ShiftOfPrecodeType);
        }
        else
        {
            throw new InvalidOperationException($"Invalid precode type width {MachineDescriptor.ReadWidthOfPrecodeType}");
        }
    }
```

After the initial precode type is determined, for stub precodes a refined precode type is extracted from the stub precode data.

```csharp
    private KnownPrecodeType? TryGetKnownPrecodeType(TargetPointer instrAddress)
    {
        // We get the precode type in two phases:
        // 1. Read the precode type from the intruction address.
        // 2. If it's "stub", look at the stub data and get the actual precode type - it could be stub,
        //    but it could also be a pinvoke precode
        // precode.h Precode::GetType()
        byte approxPrecodeType = ReadPrecodeType(instrAddress);
        byte exactPrecodeType;
        if (approxPrecodeType == MachineDescriptor.StubPrecodeType)
        {
            // get the actual type from the StubPrecodeData
            Data.StubPrecodeData stubPrecodeData = GetStubPrecodeData(instrAddress);
            exactPrecodeType = stubPrecodeData.Type;
        }
        else
        {
            exactPrecodeType = approxPrecodeType;
        }

        if (exactPrecodeType == MachineDescriptor.StubPrecodeType)
        {
            return KnownPrecodeType.Stub;
        }
        else if (MachineDescriptor.PInvokeImportPrecodeType is byte ndType && exactPrecodeType == ndType)
        {
            return KnownPrecodeType.PInvokeImport;
        }
        else if (MachineDescriptor.FixupPrecodeType is byte fixupType && exactPrecodeType == fixupType)
        {
            return KnownPrecodeType.Fixup;
        }
        else if (MachineDescriptor.ThisPointerRetBufPrecodeType is byte thisPtrRetBufType && exactPrecodeType == thisPtrRetBufType)
        {
            return KnownPrecodeType.ThisPtrRetBuf;
        }
        else
        {
            return null;
        }
    }
```

### `MethodDescFromStubAddress`

```csharp
    internal enum KnownPrecodeType
    {
        Stub = 1,
        PInvokeImport,
        Fixup,
        ThisPtrRetBuf,
        UMEntry,
        DynamicHelper,
        Interpreter
    }

    internal abstract class ValidPrecode
    {
        public TargetPointer InstrPointer { get; }
        public KnownPrecodeType PrecodeType { get; }

        protected ValidPrecode(TargetPointer instrPointer, KnownPrecodeType precodeType)
        {
            InstrPointer = instrPointer;
            PrecodeType = precodeType;
        }

        internal abstract TargetPointer GetMethodDesc(Target target, Data.PrecodeMachineDescriptor precodeMachineDescriptor);

    }

    internal class StubPrecode : ValidPrecode
    {
        internal StubPrecode(TargetPointer instrPointer, KnownPrecodeType type = KnownPrecodeType.Stub) : base(instrPointer, type) { }

        internal override TargetPointer GetMethodDesc(Target target, Data.PrecodeMachineDescriptor precodeMachineDescriptor)
        {
            TargetPointer stubPrecodeDataAddress = InstrPointer + precodeMachineDescriptor.StubCodePageSize;
            if (ContractVersion(PrecodeStubs) == 1)
                return target.ReadPointer (stubPrecodeDataAddress + /* offset of StubPrecodeData.MethodDesc */ );
            else
                return target.ReadPointer (stubPrecodeDataAddress + /* offset of StubPrecodeData.SecretParam */ );
        }
    }

    internal sealed class PInvokeImportPrecode : StubPrecode
    {
        internal PInvokeImportPrecode(TargetPointer instrPointer) : base(instrPointer, KnownPrecodeType.PInvokeImport) { }
    }

    internal sealed class FixupPrecode : ValidPrecode
    {
        internal FixupPrecode(TargetPointer instrPointer) : base(instrPointer, KnownPrecodeType.Fixup) { }
        internal override TargetPointer GetMethodDesc(Target target, Data.PrecodeMachineDescriptor precodeMachineDescriptor)
        {
            TargetPointer fixupPrecodeDataAddress = InstrPointer + precodeMachineDescriptor.StubCodePageSize;
            return target.ReadPointer (fixupPrecodeDataAddress + /* offset of FixupPrecodeData.MethodDesc */);
        }
    }

    internal sealed class ThisPtrRetBufPrecode : ValidPrecode
    {
        internal ThisPtrRetBufPrecode(TargetPointer instrPointer) : base(instrPointer, KnownPrecodeType.ThisPtrRetBuf) { }

        internal override TargetPointer GetMethodDesc(Target target, Data.PrecodeMachineDescriptor precodeMachineDescriptor)
        {
            if (ContractVersion(PrecodeStubs) == 1)
                throw new NotImplementedException(); // TODO(cdac)
            else
                return target.ReadPointer(target.ReadPointer (stubPrecodeDataAddress + /* offset of StubPrecodeData.SecretParam */ ) + /*offset of ThisPtrRetBufPrecodeData.MethodDesc*/);
        }
    }

    internal TargetPointer CodePointerReadableInstrPointer(TargetCodePointer codePointer)
    {
        // Mask off the thumb bit, if we're on arm32, to get the actual instruction pointer
        ulong instrPointer = (ulong)codePointer.AsTargetPointer & MachineDescriptor.CodePointerToInstrPointerMask.Value;
        return new TargetPointer(instrPointer);
    }


    internal ValidPrecode GetPrecodeFromEntryPoint(TargetCodePointer entryPoint)
    {
        TargetPointer instrPointer = CodePointerReadableInstrPointer(entryPoint);
        if (IsAlignedInstrPointer(instrPointer) && TryGetKnownPrecodeType(instrPointer) is KnownPrecodeType precodeType)
        {
            switch (precodeType)
            {
                case KnownPrecodeType.Stub:
                    return new StubPrecode(instrPointer);
                case KnownPrecodeType.Fixup:
                    return new FixupPrecode(instrPointer);
                case KnownPrecodeType.PInvokeImport:
                    return new PInvokeImportPrecode(instrPointer);
                case KnownPrecodeType.ThisPtrRetBuf:
                    return new ThisPtrRetBufPrecode(instrPointer);
                default:
                    break;
            }
        }
        throw new InvalidOperationException($"Invalid precode type 0x{instrPointer:x16}");
    }

    TargetPointer IPrecodeStubs.GetMethodDescFromStubAddress(TargetCodePointer entryPoint)
    {
        ValidPrecode precode = GetPrecodeFromEntryPoint(entryPoint);

        return precode.GetMethodDesc(_target, MachineDescriptor);
    }
```
