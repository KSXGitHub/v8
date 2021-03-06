// Copyright 2015 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/interpreter/bytecode-array-writer.h"

#include "src/api.h"
#include "src/interpreter/bytecode-label.h"
#include "src/interpreter/constant-array-builder.h"
#include "src/log.h"

namespace v8 {
namespace internal {
namespace interpreter {

BytecodeArrayWriter::BytecodeArrayWriter(
    Isolate* isolate, Zone* zone, ConstantArrayBuilder* constant_array_builder)
    : isolate_(isolate),
      bytecodes_(zone),
      max_register_count_(0),
      unbound_jumps_(0),
      source_position_table_builder_(isolate, zone),
      constant_array_builder_(constant_array_builder) {
  LOG_CODE_EVENT(isolate_, CodeStartLinePosInfoRecordEvent(
                               source_position_table_builder()));
}

// override
BytecodeArrayWriter::~BytecodeArrayWriter() {}

// override
Handle<BytecodeArray> BytecodeArrayWriter::ToBytecodeArray(
    int fixed_register_count, int parameter_count,
    Handle<FixedArray> handler_table) {
  DCHECK_EQ(0, unbound_jumps_);

  int bytecode_size = static_cast<int>(bytecodes()->size());

  // All locals need a frame slot for the debugger, but may not be
  // present in generated code.
  int frame_size_for_locals = fixed_register_count * kPointerSize;
  int frame_size_used = max_register_count() * kPointerSize;
  int frame_size = std::max(frame_size_for_locals, frame_size_used);
  Handle<FixedArray> constant_pool = constant_array_builder()->ToFixedArray();
  Handle<ByteArray> source_position_table =
      source_position_table_builder()->ToSourcePositionTable();
  Handle<BytecodeArray> bytecode_array = isolate_->factory()->NewBytecodeArray(
      bytecode_size, &bytecodes()->front(), frame_size, parameter_count,
      constant_pool);
  bytecode_array->set_handler_table(*handler_table);
  bytecode_array->set_source_position_table(*source_position_table);

  void* line_info = source_position_table_builder()->DetachJITHandlerData();
  LOG_CODE_EVENT(isolate_, CodeEndLinePosInfoRecordEvent(
                               AbstractCode::cast(*bytecode_array), line_info));
  return bytecode_array;
}

// override
void BytecodeArrayWriter::Write(BytecodeNode* node) {
  DCHECK(!Bytecodes::IsJump(node->bytecode()));
  UpdateSourcePositionTable(node);
  EmitBytecode(node);
}

// override
void BytecodeArrayWriter::WriteJump(BytecodeNode* node, BytecodeLabel* label) {
  DCHECK(Bytecodes::IsJump(node->bytecode()));
  UpdateSourcePositionTable(node);
  EmitJump(node, label);
}

// override
void BytecodeArrayWriter::BindLabel(BytecodeLabel* label) {
  size_t current_offset = bytecodes()->size();
  if (label->is_forward_target()) {
    // An earlier jump instruction refers to this label. Update it's location.
    PatchJump(current_offset, label->offset());
    // Now treat as if the label will only be back referred to.
  }
  label->bind_to(current_offset);
}

// override
void BytecodeArrayWriter::BindLabel(const BytecodeLabel& target,
                                    BytecodeLabel* label) {
  DCHECK(!label->is_bound());
  DCHECK(target.is_bound());
  if (label->is_forward_target()) {
    // An earlier jump instruction refers to this label. Update it's location.
    PatchJump(target.offset(), label->offset());
    // Now treat as if the label will only be back referred to.
  }
  label->bind_to(target.offset());
}

void BytecodeArrayWriter::UpdateSourcePositionTable(
    const BytecodeNode* const node) {
  int bytecode_offset = static_cast<int>(bytecodes()->size());
  const BytecodeSourceInfo& source_info = node->source_info();
  if (source_info.is_valid()) {
    source_position_table_builder()->AddPosition(bytecode_offset,
                                                 source_info.source_position(),
                                                 source_info.is_statement());
  }
}

void BytecodeArrayWriter::EmitBytecode(const BytecodeNode* const node) {
  DCHECK_NE(node->bytecode(), Bytecode::kIllegal);

  OperandScale operand_scale = node->operand_scale();
  if (operand_scale != OperandScale::kSingle) {
    Bytecode prefix = Bytecodes::OperandScaleToPrefixBytecode(operand_scale);
    bytecodes()->push_back(Bytecodes::ToByte(prefix));
  }

  Bytecode bytecode = node->bytecode();
  bytecodes()->push_back(Bytecodes::ToByte(bytecode));

  int register_operand_bitmap = Bytecodes::GetRegisterOperandBitmap(bytecode);
  const uint32_t* const operands = node->operands();
  const OperandSize* operand_sizes =
      Bytecodes::GetOperandSizes(bytecode, operand_scale);
  const OperandType* operand_types = Bytecodes::GetOperandTypes(bytecode);
  for (int i = 0; operand_types[i] != OperandType::kNone; ++i) {
    OperandType operand_type = operand_types[i];
    switch (operand_sizes[i]) {
      case OperandSize::kNone:
        UNREACHABLE();
        break;
      case OperandSize::kByte:
        bytecodes()->push_back(static_cast<uint8_t>(operands[i]));
        break;
      case OperandSize::kShort: {
        uint8_t operand_bytes[2];
        WriteUnalignedUInt16(operand_bytes, operands[i]);
        bytecodes()->insert(bytecodes()->end(), operand_bytes,
                            operand_bytes + 2);
        break;
      }
      case OperandSize::kQuad: {
        uint8_t operand_bytes[4];
        WriteUnalignedUInt32(operand_bytes, operands[i]);
        bytecodes()->insert(bytecodes()->end(), operand_bytes,
                            operand_bytes + 4);
        break;
      }
    }

    if ((register_operand_bitmap >> i) & 1) {
      int count;
      if (operand_types[i + 1] == OperandType::kRegCount) {
        count = static_cast<int>(operands[i + 1]);
      } else {
        count = Bytecodes::GetNumberOfRegistersRepresentedBy(operand_type);
      }
      Register reg = Register::FromOperand(static_cast<int32_t>(operands[i]));
      max_register_count_ = std::max(max_register_count_, reg.index() + count);
    }
  }
}

// TODO(rmcilroy): This is the same as SignedOperand in BytecodeArrayBuilder.
// Once we move the scalable operand processing here remove the SignedOperand
// in BytecodeArrayBuilder.
static uint32_t SignedOperand(int value, OperandSize size) {
  switch (size) {
    case OperandSize::kByte:
      return static_cast<uint8_t>(value & 0xff);
    case OperandSize::kShort:
      return static_cast<uint16_t>(value & 0xffff);
    case OperandSize::kQuad:
      return static_cast<uint32_t>(value);
    case OperandSize::kNone:
      UNREACHABLE();
  }
  return 0;
}

// static
Bytecode GetJumpWithConstantOperand(Bytecode jump_bytecode) {
  switch (jump_bytecode) {
    case Bytecode::kJump:
      return Bytecode::kJumpConstant;
    case Bytecode::kJumpIfTrue:
      return Bytecode::kJumpIfTrueConstant;
    case Bytecode::kJumpIfFalse:
      return Bytecode::kJumpIfFalseConstant;
    case Bytecode::kJumpIfToBooleanTrue:
      return Bytecode::kJumpIfToBooleanTrueConstant;
    case Bytecode::kJumpIfToBooleanFalse:
      return Bytecode::kJumpIfToBooleanFalseConstant;
    case Bytecode::kJumpIfNotHole:
      return Bytecode::kJumpIfNotHoleConstant;
    case Bytecode::kJumpIfNull:
      return Bytecode::kJumpIfNullConstant;
    case Bytecode::kJumpIfUndefined:
      return Bytecode::kJumpIfUndefinedConstant;
    default:
      UNREACHABLE();
      return Bytecode::kIllegal;
  }
}

void BytecodeArrayWriter::PatchJumpWith8BitOperand(size_t jump_location,
                                                   int delta) {
  Bytecode jump_bytecode = Bytecodes::FromByte(bytecodes()->at(jump_location));
  DCHECK(Bytecodes::IsJumpImmediate(jump_bytecode));
  size_t operand_location = jump_location + 1;
  DCHECK_EQ(bytecodes()->at(operand_location), 0);
  if (Bytecodes::SizeForSignedOperand(delta) == OperandSize::kByte) {
    // The jump fits within the range of an Imm operand, so cancel
    // the reservation and jump directly.
    constant_array_builder()->DiscardReservedEntry(OperandSize::kByte);
    bytecodes()->at(operand_location) = static_cast<uint8_t>(delta);
  } else {
    // The jump does not fit within the range of an Imm operand, so
    // commit reservation putting the offset into the constant pool,
    // and update the jump instruction and operand.
    size_t entry = constant_array_builder()->CommitReservedEntry(
        OperandSize::kByte, handle(Smi::FromInt(delta), isolate()));
    DCHECK(Bytecodes::SizeForUnsignedOperand(entry) == OperandSize::kByte);
    jump_bytecode = GetJumpWithConstantOperand(jump_bytecode);
    bytecodes()->at(jump_location) = Bytecodes::ToByte(jump_bytecode);
    bytecodes()->at(operand_location) = static_cast<uint8_t>(entry);
  }
}

void BytecodeArrayWriter::PatchJumpWith16BitOperand(size_t jump_location,
                                                    int delta) {
  Bytecode jump_bytecode = Bytecodes::FromByte(bytecodes()->at(jump_location));
  DCHECK(Bytecodes::IsJumpImmediate(jump_bytecode));
  size_t operand_location = jump_location + 1;
  uint8_t operand_bytes[2];
  if (Bytecodes::SizeForSignedOperand(delta) <= OperandSize::kShort) {
    constant_array_builder()->DiscardReservedEntry(OperandSize::kShort);
    WriteUnalignedUInt16(operand_bytes, static_cast<uint16_t>(delta));
  } else {
    jump_bytecode = GetJumpWithConstantOperand(jump_bytecode);
    bytecodes()->at(jump_location) = Bytecodes::ToByte(jump_bytecode);
    size_t entry = constant_array_builder()->CommitReservedEntry(
        OperandSize::kShort, handle(Smi::FromInt(delta), isolate()));
    WriteUnalignedUInt16(operand_bytes, static_cast<uint16_t>(entry));
  }
  DCHECK(bytecodes()->at(operand_location) == 0 &&
         bytecodes()->at(operand_location + 1) == 0);
  bytecodes()->at(operand_location++) = operand_bytes[0];
  bytecodes()->at(operand_location) = operand_bytes[1];
}

void BytecodeArrayWriter::PatchJumpWith32BitOperand(size_t jump_location,
                                                    int delta) {
  DCHECK(Bytecodes::IsJumpImmediate(
      Bytecodes::FromByte(bytecodes()->at(jump_location))));
  constant_array_builder()->DiscardReservedEntry(OperandSize::kQuad);
  uint8_t operand_bytes[4];
  WriteUnalignedUInt32(operand_bytes, static_cast<uint32_t>(delta));
  size_t operand_location = jump_location + 1;
  DCHECK(bytecodes()->at(operand_location) == 0 &&
         bytecodes()->at(operand_location + 1) == 0 &&
         bytecodes()->at(operand_location + 2) == 0 &&
         bytecodes()->at(operand_location + 3) == 0);
  bytecodes()->at(operand_location++) = operand_bytes[0];
  bytecodes()->at(operand_location++) = operand_bytes[1];
  bytecodes()->at(operand_location++) = operand_bytes[2];
  bytecodes()->at(operand_location) = operand_bytes[3];
}

void BytecodeArrayWriter::PatchJump(size_t jump_target, size_t jump_location) {
  Bytecode jump_bytecode = Bytecodes::FromByte(bytecodes()->at(jump_location));
  int delta = static_cast<int>(jump_target - jump_location);
  int prefix_offset = 0;
  OperandScale operand_scale = OperandScale::kSingle;
  if (Bytecodes::IsPrefixScalingBytecode(jump_bytecode)) {
    // If a prefix scaling bytecode is emitted the target offset is one
    // less than the case of no prefix scaling bytecode.
    delta -= 1;
    prefix_offset = 1;
    operand_scale = Bytecodes::PrefixBytecodeToOperandScale(jump_bytecode);
    jump_bytecode =
        Bytecodes::FromByte(bytecodes()->at(jump_location + prefix_offset));
  }

  DCHECK(Bytecodes::IsJump(jump_bytecode));
  switch (operand_scale) {
    case OperandScale::kSingle:
      PatchJumpWith8BitOperand(jump_location, delta);
      break;
    case OperandScale::kDouble:
      PatchJumpWith16BitOperand(jump_location + prefix_offset, delta);
      break;
    case OperandScale::kQuadruple:
      PatchJumpWith32BitOperand(jump_location + prefix_offset, delta);
      break;
    default:
      UNREACHABLE();
  }
  unbound_jumps_--;
}

void BytecodeArrayWriter::EmitJump(BytecodeNode* node, BytecodeLabel* label) {
  DCHECK(Bytecodes::IsJump(node->bytecode()));
  DCHECK_EQ(0, node->operand(0));

  size_t current_offset = bytecodes()->size();

  if (label->is_bound()) {
    CHECK_GE(current_offset, label->offset());
    CHECK_LE(current_offset, static_cast<size_t>(kMaxInt));
    // Label has been bound already so this is a backwards jump.
    size_t abs_delta = current_offset - label->offset();
    int delta = -static_cast<int>(abs_delta);
    OperandSize operand_size = Bytecodes::SizeForSignedOperand(delta);
    if (operand_size > OperandSize::kByte) {
      // Adjust for scaling byte prefix for wide jump offset.
      DCHECK_LE(delta, 0);
      delta -= 1;
    }
    node->set_bytecode(node->bytecode(), SignedOperand(delta, operand_size),
                       Bytecodes::OperandSizesToScale(operand_size));
  } else {
    // The label has not yet been bound so this is a forward reference
    // that will be patched when the label is bound. We create a
    // reservation in the constant pool so the jump can be patched
    // when the label is bound. The reservation means the maximum size
    // of the operand for the constant is known and the jump can
    // be emitted into the bytecode stream with space for the operand.
    unbound_jumps_++;
    label->set_referrer(current_offset);
    OperandSize reserved_operand_size =
        constant_array_builder()->CreateReservedEntry();
    OperandScale operand_scale =
        Bytecodes::OperandSizesToScale(reserved_operand_size);
    node->set_bytecode(node->bytecode(), 0, operand_scale);
  }
  EmitBytecode(node);
}

}  // namespace interpreter
}  // namespace internal
}  // namespace v8
