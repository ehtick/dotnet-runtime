// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

namespace System.CodeDom
{
    public class CodeCatchClause
    {
        private CodeTypeReference _catchExceptionType;
        private string _localName;

        public CodeCatchClause() { }

        public CodeCatchClause(string localName)
        {
            _localName = localName;
        }

        public CodeCatchClause(string localName, CodeTypeReference catchExceptionType)
        {
            _localName = localName;
            _catchExceptionType = catchExceptionType;
        }

        public CodeCatchClause(string localName, CodeTypeReference catchExceptionType, params CodeStatement[] statements)
        {
            _localName = localName;
            _catchExceptionType = catchExceptionType;
            Statements.AddRange(statements);
        }

        public string LocalName
        {
            get => _localName ?? string.Empty;
            set => _localName = value;
        }

        public CodeTypeReference CatchExceptionType
        {
            get => _catchExceptionType ??= new CodeTypeReference(typeof(Exception));
            set => _catchExceptionType = value;
        }

        public CodeStatementCollection Statements => field ??= new CodeStatementCollection();
    }
}
