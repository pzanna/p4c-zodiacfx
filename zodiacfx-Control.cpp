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

#include "zodiacfx-Control.h"
#include "zodiacfx-Type.h"
#include "zodiacfx-Table.h"
#include "lib/error.h"
#include "frontends/p4/tableApply.h"
#include "frontends/p4/typeMap.h"
#include "frontends/p4/methodInstance.h"
#include "frontends/p4/parameterSubstitution.h"

namespace ZODIACFX {

ControlBodyTranslator::ControlBodyTranslator(const ZODIACFXControl* control) :
        CodeGenInspector(control->program->refMap, control->program->typeMap), control(control),
        p4lib(P4::P4CoreLibrary::instance)
{ setName("ControlBodyTranslator"); }

bool ControlBodyTranslator::preorder(const IR::PathExpression* expression) {
    auto decl = control->program->refMap->getDeclaration(expression->path, true);
    auto param = decl->getNode()->to<IR::Parameter>();
    if (param != nullptr) {
        if (toDereference.count(param) > 0)
            builder->append("*");
        auto subst = ::get(substitution, param);
        if (subst != nullptr) {
            builder->append(subst->name);
            return false;
        }
    }
    builder->append(expression->path->name);  // each identifier should be unique
    return false;
}

void ControlBodyTranslator::processFunction(const P4::ExternFunction* function) {
    ::error("%1%: Not supported", function->method);
}

bool ControlBodyTranslator::preorder(const IR::MethodCallExpression* expression) {
    builder->append("/* ");
    visit(expression->method);
    builder->append("(");
    bool first = true;
    for (auto a  : *expression->arguments) {
        if (!first)
            builder->append(", ");
        first = false;
        visit(a);
    }
    builder->append(")");
    builder->append("*/");
    builder->newline();

    auto mi = P4::MethodInstance::resolve(expression,
                                          control->program->refMap,
                                          control->program->typeMap);
    auto apply = mi->to<P4::ApplyMethod>();
    if (apply != nullptr) {
        processApply(apply);
        return false;
    }
    auto ef = mi->to<P4::ExternFunction>();
    if (ef != nullptr) {
        processFunction(ef);
        return false;
    }
    auto bim = mi->to<P4::BuiltInMethod>();
    if (bim != nullptr) {
        builder->emitIndent();
        if (bim->name == IR::Type_Header::isValid) {
            visit(bim->appliedTo);
            builder->append(".zodiacfx_valid");
            return false;
        } else if (bim->name == IR::Type_Header::setValid) {
            visit(bim->appliedTo);
            builder->append(".zodiacfx_valid = true");
            return false;
        } else if (bim->name == IR::Type_Header::setInvalid) {
            visit(bim->appliedTo);
            builder->append(".zodiacfx_valid = false");
            return false;
        }
    }
    auto ac = mi->to<P4::ActionCall>();
    if (ac != nullptr) {
        // Action arguments have been eliminated by the mid-end.
        BUG_CHECK(expression->arguments->size() == 0,
                  "%1%: unexpected arguments for action call", expression);
        visit(ac->action->body);
        return false;
    }

    ::error("Unsupported method invocation %1%", expression);
    return false;
}

void ControlBodyTranslator::compileEmitField(const IR::Expression* expr, cstring field, unsigned alignment, ZODIACFXType* type) {
    unsigned widthToEmit = dynamic_cast<IHasWidth*>(type)->widthInBits();
    cstring swap = "";
    if (widthToEmit == 16)
        swap = "htons";
    else if (widthToEmit == 32)
        swap = "htonl";
    if (!swap.isNullOrEmpty()) {
        builder->emitIndent();
        visit(expr);
        builder->appendFormat(".%s = %s(", field.c_str(), swap);
        visit(expr);
        builder->appendFormat(".%s)", field.c_str());
        builder->endOfStatement(true);
    }

    auto program = control->program;
    unsigned bitsInFirstByte = widthToEmit % 8;
    if (bitsInFirstByte == 0) bitsInFirstByte = 8;
    unsigned bitsInCurrentByte = bitsInFirstByte;
    unsigned left = widthToEmit;

    for (unsigned i=0; i < (widthToEmit + 7) / 8; i++) {
        builder->emitIndent();
        builder->appendFormat("%s = ((char*)(&", program->byteVar.c_str());
        visit(expr);
        builder->appendFormat(".%s))[%d]", field.c_str(), i);
        builder->endOfStatement(true);

        unsigned freeBits = alignment == 0 ? (8 - alignment) : 8;
        unsigned bitsToWrite = bitsInCurrentByte > freeBits ? freeBits : bitsInCurrentByte;

        BUG_CHECK((bitsToWrite > 0) && (bitsToWrite <= 8), "invalid bitsToWrite %d", bitsToWrite);
        builder->emitIndent();
        if (alignment == 0)
            builder->appendFormat("write_byte(%s, BYTES(%s) + %d, (%s) << %d)",
                                  " ", program->offsetVar.c_str(), i,
                                  program->byteVar.c_str(), 8 - bitsToWrite);
        else
            builder->appendFormat("write_partial(%s + BYTES(%s) + %d, %d, (%s) << %d)",
                                  " ", program->offsetVar.c_str(), i,
                                  alignment,
                                  program->byteVar.c_str(), 8 - bitsToWrite);
        builder->endOfStatement(true);
        left -= bitsToWrite;
        bitsInCurrentByte -= bitsToWrite;

        if (bitsInCurrentByte > 0) {
            builder->emitIndent();
            builder->appendFormat(
                "write_byte(%s, BYTES(%s) + %d + 1, (%s << %d))",
                " ",
                program->offsetVar.c_str(), i, program->byteVar.c_str(), 8 - alignment % 8);
            builder->endOfStatement(true);
            left -= bitsInCurrentByte;
        }

        alignment = (alignment + bitsToWrite) % 8;
        bitsInCurrentByte = left >= 8 ? 8 : left;
    }

    builder->emitIndent();
    builder->appendFormat("%s += %d", program->offsetVar.c_str(), widthToEmit);
    builder->endOfStatement(true);
}

void ControlBodyTranslator::compileEmit(const IR::Vector<IR::Argument>* args) {
    BUG_CHECK(args->size() == 1, "%1%: expected 1 argument for emit", args);

    auto expr = args->at(0)->expression;
    auto type = typeMap->getType(expr);
    auto ht = type->to<IR::Type_Header>();
    if (ht == nullptr) {
        ::error("Cannot emit a non-header type %1%", expr);
        return;
    }

    auto program = control->program;
    builder->emitIndent();
    builder->append("if (");
    visit(expr);
    builder->append(".zodiacfx_valid) ");
    builder->blockStart();

    unsigned width = ht->width_bits();
    builder->emitIndent();
    builder->appendFormat("if (%s < %s + BYTES(%s + %d)) ",
                          " ",
                          " ",
                          program->offsetVar.c_str(), width);
    builder->blockStart();

    builder->emitIndent();
    builder->appendFormat("%s = %s;", program->errorVar.c_str(), p4lib.packetTooShort.str());
    builder->newline();

    builder->emitIndent();
    builder->appendFormat("return %s;", builder->target->abortReturnCode().c_str());
    builder->newline();
    builder->blockEnd(true);

    unsigned alignment = 0;
    for (auto f : ht->fields) {
        auto ftype = typeMap->getType(f);
        auto etype = ZODIACFXTypeFactory::instance->create(ftype);
        auto et = dynamic_cast<IHasWidth*>(etype);
        if (et == nullptr) {
            ::error("Only headers with fixed widths supported %1%", f);
            return;
        }
        compileEmitField(expr, f->name, alignment, etype);
        alignment += et->widthInBits();
        alignment %= 8;
    }

    builder->blockEnd(true);
    return;
}

void ControlBodyTranslator::processApply(const P4::ApplyMethod* method) {
    builder->emitIndent();
    auto table = control->getTable(method->object->getName().name);
    BUG_CHECK(table != nullptr, "No table for %1%", method->expr);

    P4::ParameterSubstitution binding;
    cstring actionVariableName;
    if (!saveAction.empty()) {
        actionVariableName = saveAction.at(saveAction.size() - 1);
        if (!actionVariableName.isNullOrEmpty()) {
            builder->appendFormat("enum %s %s;\n",
                                  table->actionEnumName.c_str(), actionVariableName.c_str());
            builder->emitIndent();
        }
    }
    builder->blockStart();

    BUG_CHECK(method->expr->arguments->size() == 0, "%1%: table apply with arguments", method);
    cstring keyname = "key";
    if (table->keyGenerator != nullptr) {
        builder->emitIndent();
        builder->appendLine("/* construct key */");
        builder->emitIndent();
        builder->appendFormat("struct %s %s = {}", table->keyTypeName.c_str(), keyname.c_str());
        builder->endOfStatement(true);
        table->emitKey(builder, keyname);
    }
    builder->emitIndent();
    builder->appendLine("/* value */");
    builder->emitIndent();
    cstring valueName = "value";
    builder->appendFormat("struct %s *%s = NULL", table->valueTypeName.c_str(), valueName.c_str());
    builder->endOfStatement(true);

    if (table->keyGenerator != nullptr) {
        builder->emitIndent();
        builder->appendLine("/* perform lookup */");
        builder->emitIndent();
        builder->target->emitTableLookup(builder, table->dataMapName, keyname, valueName);
        builder->endOfStatement(true);
    }

    builder->emitIndent();
    builder->appendFormat("if (%s == NULL) ", valueName.c_str());
    builder->blockStart();

    builder->emitIndent();
    builder->appendLine("/* miss; find default action */");
    builder->emitIndent();
    builder->appendFormat("%s = 0", control->hitVariable.c_str());
    builder->endOfStatement(true);

    builder->emitIndent();
    builder->target->emitTableLookup(builder, table->defaultActionMapName, control->program->zeroKey, valueName);
    builder->endOfStatement(true);
    builder->blockEnd(false);
    builder->append(" else ");
    builder->blockStart();
    builder->emitIndent();
    builder->appendFormat("%s = 1", control->hitVariable.c_str());
    builder->endOfStatement(true);
    builder->blockEnd(true);

    builder->emitIndent();
    builder->appendFormat("if (%s != NULL) ", valueName.c_str());
    builder->blockStart();
    builder->emitIndent();
    builder->appendLine("/* run action */");
    table->emitAction(builder, valueName);
    if (!actionVariableName.isNullOrEmpty()) {
        builder->emitIndent();
        builder->appendFormat("%s = %s->action", actionVariableName.c_str(), valueName.c_str());
        builder->endOfStatement(true);
    }
    toDereference.clear();

    builder->blockEnd(true);
    builder->emitIndent();
    builder->appendFormat("else return %s", builder->target->abortReturnCode().c_str());
    builder->endOfStatement(true);

    builder->blockEnd(true);
}

bool ControlBodyTranslator::preorder(const IR::ExitStatement*) {
    builder->appendFormat("goto %s;", control->program->endLabel.c_str());
    return false;
}

bool ControlBodyTranslator::preorder(const IR::ReturnStatement*) {
    builder->appendFormat("goto %s;", control->program->endLabel.c_str());
    return false;
}

bool ControlBodyTranslator::preorder(const IR::IfStatement* statement) {
    bool isHit = P4::TableApplySolver::isHit(statement->condition, control->program->refMap, control->program->typeMap);
    if (isHit) {
        // visit first the table, and then the conditional
        auto member = statement->condition->to<IR::Member>();
        CHECK_NULL(member);
        visit(member->expr);  // table application.  Sets 'hitVariable'
        builder->emitIndent();
    }

    // This is almost the same as the base class method
    builder->append("if (");
    if (isHit)
        builder->append(control->hitVariable);
    else
        visit(statement->condition);
    builder->append(") ");
    if (!statement->ifTrue->is<IR::BlockStatement>()) {
        builder->increaseIndent();
        builder->newline();
        builder->emitIndent();
    }
    visit(statement->ifTrue);
    if (!statement->ifTrue->is<IR::BlockStatement>())
        builder->decreaseIndent();
    if (statement->ifFalse != nullptr) {
        builder->newline();
        builder->emitIndent();
        builder->append("else ");
        if (!statement->ifFalse->is<IR::BlockStatement>()) {
            builder->increaseIndent();
            builder->newline();
            builder->emitIndent();
        }
        visit(statement->ifFalse);
        if (!statement->ifFalse->is<IR::BlockStatement>())
            builder->decreaseIndent();
    }
    return false;
}

bool ControlBodyTranslator::preorder(const IR::SwitchStatement* statement) {
    cstring newName = control->program->refMap->newName("action_run");
    saveAction.push_back(newName);
    // This must be a table.apply().action_run
    auto mem = statement->expression->to<IR::Member>();
    BUG_CHECK(mem != nullptr,
              "%1%: Unexpected expression in switch statement", statement->expression);
    visit(mem->expr);
    saveAction.pop_back();
    saveAction.push_back(nullptr);
    builder->emitIndent();
    builder->append("switch (");
    builder->append(newName);
    builder->append(") ");
    builder->blockStart();
    for (auto c : statement->cases) {
        builder->emitIndent();
        if (c->label->is<IR::DefaultExpression>()) {
            builder->append("default");
        } else {
            builder->append("case ");
            auto pe = c->label->to<IR::PathExpression>();
            auto decl = control->program->refMap->getDeclaration(pe->path, true);
            BUG_CHECK(decl->is<IR::P4Action>(), "%1%: expected an action", pe);
            auto act = decl->to<IR::P4Action>();
            cstring name = ZODIACFXObject::externalName(act);
            builder->append(name);
        }
        builder->append(":");
        builder->newline();
        builder->emitIndent();
        visit(c->statement);
        builder->newline();
        builder->emitIndent();
        builder->appendLine("break;");
    }
    builder->blockEnd(false);
    saveAction.pop_back();
    return false;
}

/////////////////////////////////////////////////

ZODIACFXControl::ZODIACFXControl(const ZODIACFXProgram* program, const IR::ControlBlock* block, const IR::Parameter* parserHeaders) :
        program(program), controlBlock(block), headers(nullptr),
        accept(nullptr), parserHeaders(parserHeaders), codeGen(nullptr) {}

void ZODIACFXControl::scanConstants() {
    for (auto c : controlBlock->constantValue) {
        auto b = c.second;
        if (!b->is<IR::Block>()) continue;
        if (b->is<IR::TableBlock>()) {
            auto tblblk = b->to<IR::TableBlock>();
            auto tbl = new ZODIACFXTable(program, tblblk, codeGen);
            tables.emplace(tblblk->container->name, tbl);
        } else {
            ::error("Unexpected block %s nested within control", b->toString());
        }
    }
}

bool ZODIACFXControl::build() {
    hitVariable = program->refMap->newName("hit");
    auto pl = controlBlock->container->type->applyParams;
    if (pl->size() != 3) {
        ::error("Expected control block to have exactly 3 parameters");
        return false;
    }

    auto it = pl->parameters.begin();
    headers = *it;
    ++it;
    inputMeta = *it;
    ++it;
    outputMeta = *it;

    codeGen = new ControlBodyTranslator(this);
    codeGen->substitute(headers, parserHeaders);

    scanConstants();
    return ::errorCount() == 0;
}

void ZODIACFXControl::emitDeclaration(CodeBuilder* builder, const IR::Declaration* decl) {
    if (decl->is<IR::Declaration_Variable>()) {
        auto vd = decl->to<IR::Declaration_Variable>();
        auto etype = ZODIACFXTypeFactory::instance->create(vd->type);
        builder->emitIndent();
        etype->declare(builder, vd->name, false);
        builder->endOfStatement(true);
        BUG_CHECK(vd->initializer == nullptr,
                  "%1%: declarations with initializers not supported", decl);
        return;
    } else if (decl->is<IR::P4Table>() ||
               decl->is<IR::P4Action>() ||
               decl->is<IR::Declaration_Instance>()) {
        return;
    }
    BUG("%1%: not yet handled", decl);
}

void ZODIACFXControl::emit(CodeBuilder* builder) {
    auto hitType = ZODIACFXTypeFactory::instance->create(IR::Type_Boolean::get());
    builder->emitIndent();
    hitType->declare(builder, hitVariable, false);
    builder->endOfStatement(true);
    for (auto a : controlBlock->container->controlLocals)
        emitDeclaration(builder, a);

    builder->emitIndent();
    codeGen->setBuilder(builder);
    controlBlock->container->body->apply(*codeGen);
    builder->newline();
}

void ZODIACFXControl::emitTableTypes(CodeBuilder* builder) {
    for (auto it : tables)
        it.second->emitTypes(builder);
}

void ZODIACFXControl::emitTableInitializers(CodeBuilder* builder) {
    for (auto it : tables)
        it.second->emitInitializer(builder);
}

//////////////////////////////////////////////////////////////////////////

class OutHeaderSize final : public CodeGenInspector {
    P4::ReferenceMap*  refMap;
    P4::TypeMap*       typeMap;
    const ZODIACFXProgram*  program;

    std::map<const IR::Parameter*, const IR::Parameter*> substitution;

    bool illegal(const IR::Statement* statement)
    { ::error("%1%: not supported in deparser", statement); return false; }

 public:
    OutHeaderSize(P4::ReferenceMap* refMap, P4::TypeMap* typeMap, const ZODIACFXProgram* program):
            CodeGenInspector(refMap, typeMap), refMap(refMap), typeMap(typeMap), program(program) {
                CHECK_NULL(refMap); CHECK_NULL(typeMap); CHECK_NULL(program);
                setName("OutHeaderSize"); }
    bool preorder(const IR::PathExpression* expression) override {
        auto decl = refMap->getDeclaration(expression->path, true);
        auto param = decl->getNode()->to<IR::Parameter>();
        if (param != nullptr) {
            auto subst = ::get(substitution, param);
            if (subst != nullptr) {
                builder->append(subst->name);
                return false;
            }
        }
        builder->append(expression->path->name);
        return false;
    }
    bool preorder(const IR::SwitchStatement* statement) override
    { return illegal(statement); }
    bool preorder(const IR::IfStatement* statement) override
    { return illegal(statement); }
    bool preorder(const IR::AssignmentStatement* statement) override
    { return illegal(statement); }
    bool preorder(const IR::ReturnStatement* statement) override
    { return illegal(statement); }
    bool preorder(const IR::ExitStatement* statement) override
    { return illegal(statement); }
    bool preorder(const IR::MethodCallStatement* statement) override {
        auto &p4lib = P4::P4CoreLibrary::instance;

        auto mi = P4::MethodInstance::resolve(statement->methodCall, refMap, typeMap);
        auto method = mi->to<P4::ExternMethod>();
        if (method == nullptr)
            return illegal(statement);

        auto declType = method->originalExternType;
        if (declType->name.name != p4lib.packetOut.name ||
            method->method->name.name != p4lib.packetOut.emit.name ||
            method->expr->arguments->size() != 1) {
            return illegal(statement);
        }

        auto h = method->expr->arguments->at(0);
        auto type = typeMap->getType(h);
        auto ht = type->to<IR::Type_Header>();
        if (ht == nullptr) {
            ::error("Cannot emit a non-header type %1%", h);
            return false;
        }
        unsigned width = ht->width_bits();

        builder->append("if (");
        visit(h);
        builder->append(".zodiacfx_valid) ");
        builder->appendFormat("%s += %d;", program->outHeaderLengthVar.c_str(), width);
        return false;
    }

    void substitute(const IR::Parameter* p, const IR::Parameter* with)
    { substitution.emplace(p, with); }
};

/////////////////////////////////////////////////

ZODIACFXDeparser::ZODIACFXDeparser(const ZODIACFXProgram* program, const IR::ControlBlock* block, const IR::Parameter* parserHeaders) :
        ZODIACFXControl(program, block, parserHeaders), packet(nullptr) {}

bool ZODIACFXDeparser::build() {
    auto pl = controlBlock->container->type->applyParams;
    if (pl->size() != 3) {
        ::error("Expected deparser block to have exactly 3 parameters");
        return false;
    }

    auto it = pl->parameters.begin();
    headers = *it;
    ++it;
    packet = *it;

    codeGen = new ControlBodyTranslator(this);
    codeGen->substitute(headers, parserHeaders);

    return true;
}

void ZODIACFXDeparser::emit(CodeBuilder* builder) {
    OutHeaderSize ohs(program->refMap, program->typeMap,static_cast<const ZODIACFXProgram*>(program));
    ohs.substitute(headers, parserHeaders);
    ohs.setBuilder(builder);

    builder->emitIndent();
    //(void)controlBlock->container->body->apply(ohs);
    builder->newline();
}

}  // namespace ZODIACFX
