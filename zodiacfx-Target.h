/*
Copyright 2019 Northbound Networks.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#ifndef _BACKENDS_ZODIACFX_TARGET_H_
#define _BACKENDS_ZODIACFX_TARGET_H_

#include "lib/cstring.h"
#include "lib/sourceCodeBuilder.h"
#include "lib/exceptions.h"

// We are prepared to support code generation using multiple styles
// (e.g., using BCC or using CLANG).

namespace ZODIACFX {

class Target {
 protected:
    explicit Target(cstring name) : name(name) {}
    Target() = delete;
    virtual ~Target() {}

 public:
    const cstring name;

    virtual void emitLicense(Util::SourceCodeBuilder* builder, cstring license) const = 0;
    virtual void emitCodeSection(Util::SourceCodeBuilder* builder, cstring sectionName) const = 0;
    virtual void emitIncludes(Util::SourceCodeBuilder* builder) const = 0;
    virtual void emitTableLookup(Util::SourceCodeBuilder* builder, cstring tblName, cstring key, cstring value) const = 0;
    virtual void emitTableUpdate(Util::SourceCodeBuilder* builder, cstring tblName, cstring key, cstring value) const = 0;
    virtual void emitUserTableUpdate(Util::SourceCodeBuilder* builder, cstring tblName, cstring key, cstring value) const = 0;
    virtual void emitTableDecl(Util::SourceCodeBuilder* builder, cstring tblName, bool isHash, cstring keyType, cstring valueType, unsigned size) const = 0;
    virtual void emitMain(Util::SourceCodeBuilder* builder, cstring functionName, cstring argName, cstring packetSize) const = 0;
    virtual cstring dataOffset(cstring base) const = 0;
    virtual cstring dataEnd(cstring base) const = 0;
    virtual cstring forwardReturnCode() const = 0;
    virtual cstring dropReturnCode() const = 0;
    virtual cstring abortReturnCode() const = 0;
};

// Represents a target compiled by bcc that uses the TC
class zodiacfxTarget : public Target {
 public:
    zodiacfxTarget() : Target("zodiacfx") {}
    void emitLicense(Util::SourceCodeBuilder*, cstring) const override {};
    void emitCodeSection(Util::SourceCodeBuilder*, cstring) const override {}
    void emitIncludes(Util::SourceCodeBuilder* builder) const override;
    void emitTableLookup(Util::SourceCodeBuilder* builder, cstring tblName, cstring key, cstring value) const override;
    void emitTableUpdate(Util::SourceCodeBuilder* builder, cstring tblName, cstring key, cstring value) const override;
    void emitUserTableUpdate(Util::SourceCodeBuilder* builder, cstring tblName, cstring key, cstring value) const override;
    void emitTableDecl(Util::SourceCodeBuilder* builder, cstring tblName, bool isHash, cstring keyType, cstring valueType, unsigned size) const override;
    void emitMain(Util::SourceCodeBuilder* builder, cstring functionName, cstring argName, cstring packetSize) const override;
    cstring dataOffset(cstring base) const override { return base; }
    cstring dataEnd(cstring base) const override
    { return cstring("(") + base + " + " + base + "->len)"; }
    cstring forwardReturnCode() const override { return "0"; }
    cstring dropReturnCode() const override { return "1"; }
    cstring abortReturnCode() const override { return "1"; }
};

}  // namespace ZODIACFX

#endif /* _BACKENDS_ZODIACFX_TARGET_H_ */
