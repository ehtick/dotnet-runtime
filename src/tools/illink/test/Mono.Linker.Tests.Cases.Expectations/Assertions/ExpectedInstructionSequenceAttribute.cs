// Copyright (c) .NET Foundation and contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

using System;

namespace Mono.Linker.Tests.Cases.Expectations.Assertions
{
    [AttributeUsage(AttributeTargets.Method | AttributeTargets.Constructor, Inherited = false, AllowMultiple = false)]
    public class ExpectedInstructionSequenceAttribute : BaseInAssemblyAttribute
    {
        public ExpectedInstructionSequenceAttribute(string[] opCodes)
        {
            ArgumentNullException.ThrowIfNull(opCodes);
        }
    }
}
