#include "ast/ast.h"

namespace sorbet::ast {

class Verifier {
public:
    static ExprPtr run(core::Context ctx, ExprPtr node);
};

} // namespace sorbet::ast
