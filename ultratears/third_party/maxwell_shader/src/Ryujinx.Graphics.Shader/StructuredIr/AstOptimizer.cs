using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using System.Collections.Generic;
using System.Linq;

using static Ryujinx.Graphics.Shader.StructuredIr.AstHelper;

namespace Ryujinx.Graphics.Shader.StructuredIr
{
    static class AstOptimizer
    {
        public static void Optimize(StructuredProgramContext context)
        {
            AstBlock mainBlock = context.CurrentFunction.MainBlock;

            // When debug mode is enabled, we disable expression propagation
            // (this makes comparison with the disassembly easier).
            if (!context.DebugMode)
            {
                AstBlockVisitor visitor = new(mainBlock);

                foreach (IAstNode node in visitor.Visit())
                {
                    if (node is AstAssignment assignment && assignment.Destination is AstOperand propVar)
                    {
                        // TOTK (2026-10-16): an atomic / call / image atomic whose result is unused (Maxwell RED, ATOM with
                        // an RZ destination) stays a statement in place: propagating it would move the side effect to its
                        // use, and removing the assignment removed the operation itself (the reduction kernel c7830fac
                        // lost its atomicMin on 2026-10-13; every translation ran in DebugMode since, to avoid this).
                        bool sideEffects = HasSideEffects(assignment.Source);
                        bool isWorthPropagating = !sideEffects && (propVar.Uses.Count == 1 || IsWorthPropagating(assignment.Source));

                        // TOTK: SPIR-V passes call arguments by pointer to a variable (StructuredProgram copies them
                        // into temporaries); folding an expression into a call argument breaks GenerateCall.
                        bool usedByCall = propVar.Uses.Any(u => u is AstOperation op && op.Inst == Instruction.Call);

                        if (propVar.Defs.Count == 1 && isWorthPropagating && !usedByCall)
                        {
                            PropagateExpression(propVar, assignment.Source);
                        }

                        if (propVar.Type == OperandType.LocalVariable && propVar.Uses.Count == 0 && !sideEffects)
                        {
                            visitor.Block.Remove(assignment);

                            context.CurrentFunction.Locals.Remove(propVar);
                        }
                    }
                }
            }

            RemoveEmptyBlocks(mainBlock);
        }

        private static bool HasSideEffects(IAstNode source)
        {
            if (source is AstOperation op)
            {
                switch (op.Inst & Instruction.Mask)
                {
                    case Instruction.AtomicAdd:
                    case Instruction.AtomicAnd:
                    case Instruction.AtomicCompareAndSwap:
                    case Instruction.AtomicMaxS32:
                    case Instruction.AtomicMaxU32:
                    case Instruction.AtomicMinS32:
                    case Instruction.AtomicMinU32:
                    case Instruction.AtomicOr:
                    case Instruction.AtomicSwap:
                    case Instruction.AtomicXor:
                    case Instruction.Call:
                    case Instruction.ImageAtomic:
                        return true;
                }
            }

            return false;
        }

        private static bool IsWorthPropagating(IAstNode source)
        {
            if (source is not AstOperation srcOp)
            {
                return false;
            }

            if (!InstructionInfo.IsUnary(srcOp.Inst))
            {
                return false;
            }

            return srcOp.GetSource(0) is AstOperand || srcOp.Inst == Instruction.Copy;
        }

        private static void PropagateExpression(AstOperand propVar, IAstNode source)
        {
            IAstNode[] uses = propVar.Uses.ToArray();

            foreach (IAstNode useNode in uses)
            {
                if (useNode is AstBlock useBlock)
                {
                    useBlock.Condition = source;
                }
                else if (useNode is AstOperation useOperation)
                {
                    for (int srcIndex = 0; srcIndex < useOperation.SourcesCount; srcIndex++)
                    {
                        if (useOperation.GetSource(srcIndex) == propVar)
                        {
                            useOperation.SetSource(srcIndex, source);
                        }
                    }
                }
                else if (useNode is AstAssignment useAssignment)
                {
                    useAssignment.Source = source;
                }
            }
        }

        private const bool ElseIfFlattening = false;

        private static void RemoveEmptyBlocks(AstBlock mainBlock)
        {
            Queue<AstBlock> pending = new();

            pending.Enqueue(mainBlock);

            while (pending.TryDequeue(out AstBlock block))
            {
                foreach (IAstNode node in block)
                {
                    if (node is AstBlock childBlock)
                    {
                        pending.Enqueue(childBlock);
                    }
                }

                AstBlock parent = block.Parent;

                if (parent == null)
                {
                    continue;
                }

                AstBlock nextBlock = Next(block) as AstBlock;

                bool hasElse = nextBlock != null && nextBlock.Type == AstBlockType.Else;

                bool isIf = block.Type == AstBlockType.If;

                if (block.Count == 0)
                {
                    if (isIf)
                    {
                        if (hasElse)
                        {
                            nextBlock.TurnIntoIf(InverseCond(block.Condition));
                        }

                        parent.Remove(block);
                    }
                    else if (block.Type == AstBlockType.Else)
                    {
                        parent.Remove(block);
                    }
                }
                // TOTK (2026-10-16): the else-if flattening (`else { if (c) A else B }` -> `else if (c) A else B`) is
                // disabled. The SPIR-V backend gives the else-if the same merge block as the if before it, and
                // SPIRV-Cross then drops the trailing else of the chain: TOTK's fragment shader 1870878a lost three
                // loops with an atomic max (a feedback write) and a discard (scripts/diag/msl_invariants.py found 12
                // variants losing atomics and 83 changing discards). The nested form is what DebugMode always produced,
                // because its comment nodes keep every block non-empty and this branch never ran.
                else if (ElseIfFlattening && isIf && parent.Type == AstBlockType.Else && parent.Count == (hasElse ? 2 : 1))
                {
                    AstBlock parentOfParent = parent.Parent;

                    parent.Remove(block);

                    parentOfParent.AddAfter(parent, block);

                    if (hasElse)
                    {
                        parent.Remove(nextBlock);

                        parentOfParent.AddAfter(block, nextBlock);
                    }

                    parentOfParent.Remove(parent);

                    block.TurnIntoElseIf();
                }
            }
        }
    }
}
