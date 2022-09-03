// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Limited.

#include <glog/logging.h>
#include <gtest/gtest.h>
#include <math.h>

#include "butil/time.h"
#include "column/column_helper.h"
#include "column/fixed_length_column.h"
#include "exprs/vectorized/arithmetic_expr.h"
#include "exprs/vectorized/array_expr.h"
#include "exprs/vectorized/array_map_expr.h"
#include "exprs/vectorized/cast_expr.h"
#include "exprs/vectorized/function_call_expr.h"
#include "exprs/vectorized/is_null_predicate.h"
#include "exprs/vectorized/lambda_function.h"
#include "exprs/vectorized/literal.h"
#include "exprs/vectorized/mock_vectorized_expr.h"

namespace starrocks {
namespace vectorized {

class FakeConstExpr : public starrocks::Expr {
public:
    explicit FakeConstExpr(const TExprNode& dummy) : Expr(dummy) {}

    ColumnPtr evaluate(ExprContext*, Chunk*) override { return _column; }

    Expr* clone(ObjectPool*) const override { return nullptr; }

    ColumnPtr _column;
};

ColumnPtr build_int_column(const std::vector<int>& values) {
    auto data = Int32Column::create();
    data->append_numbers(values.data(), values.size() * sizeof(int32_t));
    return data;
}

ColumnPtr build_int_column(const std::vector<int>& values, const std::vector<uint8_t>& nullflags) {
    DCHECK_EQ(values.size(), nullflags.size());
    auto null = NullColumn::create();
    null->append_numbers(nullflags.data(), nullflags.size());

    auto data = build_int_column(values);

    return NullableColumn::create(std::move(data), std::move(null));
}

class VectorizedLambdaFunctionExprTest : public ::testing::Test {
public:
    void SetUp() {
        // init the int_type.
        TTypeNode node;
        node.__set_type(TTypeNodeType::SCALAR);
        TScalarType scalar_type;
        scalar_type.__set_type(TPrimitiveType::INT);
        node.__set_scalar_type(scalar_type);
        int_type.types.push_back(node);

        // init expr_node
        expr_node.opcode = TExprOpcode::ADD;
        expr_node.child_type = TPrimitiveType::INT;
        expr_node.node_type = TExprNodeType::BINARY_PRED;
        expr_node.num_children = 2;
        expr_node.__isset.opcode = true;
        expr_node.__isset.child_type = true;
        expr_node.type = gen_type_desc(TPrimitiveType::BOOLEAN);

        create_array_expr();
        create_lambda_expr();
    }

    void create_lambda_expr() {
        // create lambda functions
        TExprNode tlambda_func;
        tlambda_func.opcode = TExprOpcode::ADD;
        tlambda_func.child_type = TPrimitiveType::INT;
        tlambda_func.node_type = TExprNodeType::LAMBDA_FUNCTION_EXPR;
        tlambda_func.num_children = 2;
        tlambda_func.__isset.opcode = true;
        tlambda_func.__isset.child_type = true;
        tlambda_func.type = gen_type_desc(TPrimitiveType::INT);
        LambdaFunction* lambda_func = _objpool.add(new LambdaFunction(tlambda_func));

        // x -> x
        TExprNode slot_ref;
        slot_ref.node_type = TExprNodeType::SLOT_REF;
        slot_ref.type = int_type;
        slot_ref.num_children = 0;
        slot_ref.__isset.slot_ref = true;
        slot_ref.slot_ref.slot_id = 100000;
        slot_ref.slot_ref.tuple_id = 0;
        slot_ref.__set_use_vectorized(true);
        slot_ref.__set_is_nullable(true);

        ColumnRef* col1 = _objpool.add(new ColumnRef(slot_ref));
        ColumnRef* col2 = _objpool.add(new ColumnRef(slot_ref));
        lambda_func->add_child(col1);
        lambda_func->add_child(col2);
        _lambda_func.push_back(lambda_func);

        // x -> x is null
        lambda_func = _objpool.add(new LambdaFunction(tlambda_func));
        ColumnRef* col3 = _objpool.add(new ColumnRef(slot_ref));
        ColumnRef* col4 = _objpool.add(new ColumnRef(slot_ref));
        expr_node.fn.name.function_name = "is_null_pred";
        auto* is_null = _objpool.add(VectorizedIsNullPredicateFactory::from_thrift(expr_node));
        is_null->add_child(col4);
        lambda_func->add_child(is_null);
        lambda_func->add_child(col3);
        _lambda_func.push_back(lambda_func);

        // x -> x + a (captured columns)
        lambda_func = _objpool.add(new LambdaFunction(tlambda_func));
        ColumnRef* col5 = _objpool.add(new ColumnRef(slot_ref));
        expr_node.opcode = TExprOpcode::ADD;
        expr_node.type = gen_type_desc(TPrimitiveType::INT);
        auto* add_expr = _objpool.add(VectorizedArithmeticExprFactory::from_thrift(expr_node));
        ColumnRef* col6 = _objpool.add(new ColumnRef(slot_ref));
        slot_ref.slot_ref.slot_id = 1;
        ColumnRef* col7 = _objpool.add(new ColumnRef(slot_ref));
        add_expr->_children.push_back(col6);
        add_expr->_children.push_back(col7);
        lambda_func->add_child(add_expr);
        lambda_func->add_child(col5);
        _lambda_func.push_back(lambda_func);

        // x -> -110
        lambda_func = _objpool.add(new LambdaFunction(tlambda_func));
        auto tint_literal = create_int_literal_node(-110);
        auto int_literal = _objpool.add(new VectorizedLiteral(tint_literal));
        slot_ref.slot_ref.slot_id = 100000;
        ColumnRef* col8 = _objpool.add(new ColumnRef(slot_ref));
        lambda_func->add_child(int_literal);
        lambda_func->add_child(col8);
        _lambda_func.push_back(lambda_func);
    }

    void create_array_expr() {
        TypeDescriptor type_arr_int;
        type_arr_int.type = PrimitiveType::TYPE_ARRAY;
        type_arr_int.children.emplace_back();
        type_arr_int.children.back().type = PrimitiveType::TYPE_INT;

        // [1,4]
        // [null,null]
        // [null,12]
        auto array = ColumnHelper::create_column(type_arr_int, true);
        array->append_datum(DatumArray{Datum((int32_t)1), Datum((int32_t)4)}); // [1,4]
        array->append_datum(DatumArray{Datum(), Datum()});                     // [NULL, NULL]
        array->append_datum(DatumArray{Datum(), Datum((int32_t)12)});          // [NULL, 12]
        auto* array_values = new_fake_const_expr(array, type_arr_int);
        _array_expr.push_back(array_values);

        // null
        array = ColumnHelper::create_column(type_arr_int, true);
        array->append_datum(Datum{}); // null
        auto* const_null = new_fake_const_expr(array, type_arr_int);
        _array_expr.push_back(const_null);

        // [null]
        array = ColumnHelper::create_column(type_arr_int, true);
        array->append_datum(DatumArray{Datum()});
        auto* null_array = new_fake_const_expr(array, type_arr_int);
        _array_expr.push_back(null_array);

        // []
        array = ColumnHelper::create_column(type_arr_int, true);
        array->append_datum(DatumArray{}); // []
        auto empty_array = new_fake_const_expr(array, type_arr_int);
        _array_expr.push_back(empty_array);

        // [null]
        // []
        // NULL
        array = ColumnHelper::create_column(type_arr_int, true);
        array->append_datum(DatumArray{Datum()}); // [null]
        array->append_datum(DatumArray{});        // []
        array->append_datum(Datum{});             // NULL
        auto* array_special = new_fake_const_expr(array, type_arr_int);
        _array_expr.push_back(array_special);

        // const([1,4]...)
        array = ColumnHelper::create_column(type_arr_int, true);
        array->append_datum(DatumArray{Datum((int32_t)1), Datum((int32_t)4)}); // [1,4]
        auto const_col = ConstColumn::create(array, 3);
        auto* const_array = new_fake_const_expr(const_col, type_arr_int);
        _array_expr.push_back(const_array);

        // const(null...)
        array = ColumnHelper::create_column(type_arr_int, true);
        array->append_datum(Datum{}); // null...
        const_col = ConstColumn::create(array, 3);
        const_array = new_fake_const_expr(const_col, type_arr_int);
        _array_expr.push_back(const_array);

        // const([null]...)
        array = ColumnHelper::create_column(type_arr_int, true);
        array->append_datum(DatumArray{Datum()}); // [null]...
        const_col = ConstColumn::create(array, 3);
        const_array = new_fake_const_expr(const_col, type_arr_int);
        _array_expr.push_back(const_array);

        // const([]...)
        array = ColumnHelper::create_column(type_arr_int, true);
        array->append_datum(DatumArray{}); // []...
        const_col = ConstColumn::create(array, 3);
        const_array = new_fake_const_expr(const_col, type_arr_int);
        _array_expr.push_back(const_array);
    }
    FakeConstExpr* new_fake_const_expr(ColumnPtr value, const TypeDescriptor& type) {
        TExprNode node;
        node.__set_node_type(TExprNodeType::INT_LITERAL);
        node.__set_num_children(0);
        node.__set_type(type.to_thrift());
        FakeConstExpr* e = _objpool.add(new FakeConstExpr(node));
        e->_column = std::move(value);
        return e;
    }

    MockExpr* new_mock_expr(ColumnPtr value, const PrimitiveType& type) {
        return new_mock_expr(std::move(value), TypeDescriptor(type));
    }

    MockExpr* new_mock_expr(ColumnPtr value, const TypeDescriptor& type) {
        TExprNode node;
        node.__set_node_type(TExprNodeType::INT_LITERAL);
        node.__set_num_children(0);
        node.__set_type(type.to_thrift());
        MockExpr* e = _objpool.add(new MockExpr(node, std::move(value)));
        return e;
    }
    Expr* create_array_expr(const TTypeDesc& type) {
        TExprNode node;
        node.__set_node_type(TExprNodeType::ARRAY_EXPR);
        node.__set_is_nullable(true);
        node.__set_type(type);
        node.__set_num_children(0);

        auto* expr = _objpool.add(ArrayExprFactory::from_thrift(node));
        return expr;
    }
    TExprNode create_int_literal_node(int64_t value_literal) {
        TExprNode lit_node;
        lit_node.__set_node_type(TExprNodeType::INT_LITERAL);
        lit_node.__set_num_children(0);
        lit_node.__set_type(int_type);
        TIntLiteral lit_value;
        lit_value.__set_value(value_literal);
        lit_node.__set_int_literal(lit_value);
        lit_node.__set_use_vectorized(true);
        return lit_node;
    }

    TExprNode expr_node;
    std::vector<Expr*> _lambda_func;
    std::vector<Expr*> _array_expr;
    std::vector<Chunk*> _chunks;

private:
    TTypeDesc int_type;
    ObjectPool _objpool;
};

// just consider one level, not nested
// array_map(lambdaFunction(x<type>, lambdaExpr),array<type>)
TEST_F(VectorizedLambdaFunctionExprTest, array_map_lambda_test_normal_array) {
    auto cur_chunk = std::make_shared<vectorized::Chunk>();
    std::vector<int> vec_a = {1, 1, 1};
    cur_chunk->append_column(build_int_column(vec_a), 1);
    for (int i = 0; i < 1; ++i) {
        for (int j = 0; j < _lambda_func.size(); ++j) {
            expr_node.fn.name.function_name = "array_map";
            ArrayMapExpr array_map_expr(expr_node);
            array_map_expr.clear_children();
            array_map_expr.add_child(_lambda_func[j]);
            array_map_expr.add_child(_array_expr[i]);
            ExprContext exprContext(&array_map_expr);
            exprContext._is_clone = true;
            WARN_IF_ERROR(array_map_expr.prepare(nullptr, &exprContext), "");
            auto lambda = dynamic_cast<LambdaFunction*>(_lambda_func[j]);

            // check LambdaFunction::prepare()
            std::vector<SlotId> ids, arguments;
            lambda->get_slot_ids(&ids);
            lambda->get_lambda_arguments_ids(&arguments);

            ASSERT_TRUE(arguments.size() == 1 && arguments[0] == 100000); // the x's slot_id = 100000
            if (j == 2) {
                ASSERT_TRUE(ids.size() == 1 && ids[0] == 1); // the slot_id of the captured column is 1
            } else {
                ASSERT_TRUE(ids.empty());
            }

            WARN_IF_ERROR(array_map_expr.open(nullptr, &exprContext, FunctionContext::FunctionStateScope::THREAD_LOCAL),
                          "");

            ColumnPtr result = array_map_expr.evaluate(&exprContext, cur_chunk.get());

            if (i == 0 && j == 0) { // array_map(x -> x, array<int>)
                                    // [1,4]
                                    // [null,null]
                                    // [null,12]
                ASSERT_FALSE(result->is_constant());
                ASSERT_FALSE(result->is_numeric());

                EXPECT_EQ(3, result->size());
                EXPECT_EQ(1, result->get(0).get_array()[0].get_int32());
                EXPECT_EQ(4, result->get(0).get_array()[1].get_int32());
                ASSERT_TRUE(result->get(1).get_array()[0].is_null());
                ASSERT_TRUE(result->get(1).get_array()[1].is_null());
                ASSERT_TRUE(result->get(2).get_array()[0].is_null());
                EXPECT_EQ(12, result->get(2).get_array()[1].get_int32());
            } else if (i == 0 && j == 1) { // array_map(x -> x is null, array<int>)
                EXPECT_EQ(3, result->size());
                EXPECT_EQ(0, result->get(0).get_array()[0].get_int8());
                EXPECT_EQ(0, result->get(0).get_array()[1].get_int8());
                EXPECT_EQ(1, result->get(1).get_array()[0].get_int8());
                EXPECT_EQ(1, result->get(1).get_array()[1].get_int8());
                EXPECT_EQ(1, result->get(2).get_array()[0].get_int8());
                EXPECT_EQ(0, result->get(2).get_array()[1].get_int8());
            } else if (i == 0 && j == 2) { // // array_map(x -> x+a, array<int>)
                EXPECT_EQ(3, result->size());
                EXPECT_EQ(2, result->get(0).get_array()[0].get_int32());
                EXPECT_EQ(5, result->get(0).get_array()[1].get_int32());
                ASSERT_TRUE(result->get(1).get_array()[0].is_null());
                ASSERT_TRUE(result->get(1).get_array()[1].is_null());
                ASSERT_TRUE(result->get(2).get_array()[0].is_null());
                EXPECT_EQ(13, result->get(2).get_array()[1].get_int32());
            } else if (i == 0 && j == 3) {
                EXPECT_EQ(3, result->size());
                EXPECT_EQ(-110, result->get(0).get_array()[0].get_int32());
                EXPECT_EQ(-110, result->get(0).get_array()[1].get_int32());
                EXPECT_EQ(-110, result->get(1).get_array()[0].get_int32());
                EXPECT_EQ(-110, result->get(1).get_array()[1].get_int32());
                EXPECT_EQ(-110, result->get(2).get_array()[0].get_int32());
                EXPECT_EQ(-110, result->get(2).get_array()[1].get_int32());
            }

            exprContext.close(nullptr);
        }
    }
}

TEST_F(VectorizedLambdaFunctionExprTest, array_map_lambda_test_special_array) {
    auto cur_chunk = std::make_shared<vectorized::Chunk>();
    std::vector<int> vec_a = {1, 1, 1};
    cur_chunk->append_column(build_int_column(vec_a), 1);
    for (int i = 1; i < 5; ++i) {
        for (int j = 0; j < _lambda_func.size(); ++j) {
            expr_node.fn.name.function_name = "array_map";
            ArrayMapExpr array_map_expr(expr_node);
            array_map_expr.clear_children();
            array_map_expr.add_child(_lambda_func[j]);
            array_map_expr.add_child(_array_expr[i]);
            ExprContext exprContext(&array_map_expr);
            exprContext._is_clone = true;
            WARN_IF_ERROR(array_map_expr.prepare(nullptr, &exprContext), "");
            auto lambda = dynamic_cast<LambdaFunction*>(_lambda_func[j]);

            // check LambdaFunction::prepare()
            std::vector<SlotId> ids, arguments;
            lambda->get_slot_ids(&ids);
            lambda->get_lambda_arguments_ids(&arguments);

            ASSERT_TRUE(arguments.size() == 1 && arguments[0] == 100000); // the x's slot_id = 100000
            if (j == 2) {
                ASSERT_TRUE(ids.size() == 1 && ids[0] == 1); // the slot_id of the captured column is 1
            } else {
                ASSERT_TRUE(ids.empty());
            }

            WARN_IF_ERROR(array_map_expr.open(nullptr, &exprContext, FunctionContext::FunctionStateScope::THREAD_LOCAL),
                          "");

            ColumnPtr result = array_map_expr.evaluate(&exprContext, cur_chunk.get());

            if (i == 1) { // array_map(x->xxx,null)
                EXPECT_EQ(1, result->size());
                ASSERT_TRUE(result->is_null(0));
            } else if (i == 2 && (j == 0 || j == 2)) { // array_map( x->x || x->x+a, [null])
                EXPECT_EQ(1, result->size());
                ASSERT_TRUE(result->get(0).get_array()[0].is_null());
            } else if (i == 2 && j == 1) { // array_map(x -> x is null,[null])
                EXPECT_EQ(1, result->size());
                EXPECT_EQ(1, result->get(0).get_array()[0].get_int8());
            } else if (i == 2 && j == 3) { // array_map(x -> -110,[null])
                EXPECT_EQ(1, result->size());
                EXPECT_EQ(-110, result->get(0).get_array()[0].get_int32());
            } else if (i == 3) { // array_map(x->xxx,[])
                EXPECT_EQ(1, result->size());
                ASSERT_TRUE(result->get(0).get_array().empty());
            } else if (i == 4 && (j == 0 || j == 2)) { // array_map(x->x || x->x+a, array<special>)
                                                       // [null]
                                                       // []
                                                       // NULL
                EXPECT_EQ(3, result->size());
                ASSERT_TRUE(result->get(0).get_array()[0].is_null());
                ASSERT_TRUE(result->get(1).get_array().empty());
                ASSERT_TRUE(result->is_null(2));
            } else if (i == 4 && j == 1) { // array_map(x->x is null, array<special>)
                EXPECT_EQ(3, result->size());
                EXPECT_EQ(1, result->get(0).get_array()[0].get_int8());
                ASSERT_TRUE(result->get(1).get_array().empty());
                ASSERT_TRUE(result->is_null(2));
            } else if (i == 4 && j == 3) { // array_map(x-> -110, array<special>)
                EXPECT_EQ(3, result->size());
                EXPECT_EQ(-110, result->get(0).get_array()[0].get_int32());
                ASSERT_TRUE(result->get(1).get_array().empty());
                ASSERT_TRUE(result->is_null(2));
            }

            exprContext.close(nullptr);
        }
    }
}

TEST_F(VectorizedLambdaFunctionExprTest, array_map_lambda_test_const_array) {
    auto cur_chunk = std::make_shared<vectorized::Chunk>();
    std::vector<int> vec_a = {1, 1, 1};
    cur_chunk->append_column(build_int_column(vec_a), 1);
    for (int i = 5; i < _array_expr.size(); ++i) {
        for (int j = 0; j < _lambda_func.size(); ++j) {
            expr_node.fn.name.function_name = "array_map";
            ArrayMapExpr array_map_expr(expr_node);
            array_map_expr.clear_children();
            array_map_expr.add_child(_lambda_func[j]);
            array_map_expr.add_child(_array_expr[i]);
            ExprContext exprContext(&array_map_expr);
            exprContext._is_clone = true;
            WARN_IF_ERROR(array_map_expr.prepare(nullptr, &exprContext), "");
            auto lambda = dynamic_cast<LambdaFunction*>(_lambda_func[j]);

            // check LambdaFunction::prepare()
            std::vector<SlotId> ids, arguments;
            lambda->get_slot_ids(&ids);
            lambda->get_lambda_arguments_ids(&arguments);

            ASSERT_TRUE(arguments.size() == 1 && arguments[0] == 100000); // the x's slot_id = 100000
            if (j == 2) {
                ASSERT_TRUE(ids.size() == 1 && ids[0] == 1); // the slot_id of the captured column is 1
            } else {
                ASSERT_TRUE(ids.empty());
            }

            WARN_IF_ERROR(array_map_expr.open(nullptr, &exprContext, FunctionContext::FunctionStateScope::THREAD_LOCAL),
                          "");

            ColumnPtr result = array_map_expr.evaluate(&exprContext, cur_chunk.get());

            if (i == 5 && j == 0) { // array_map( x->x, array<const[1,4]...>)
                EXPECT_EQ(3, result->size());
                EXPECT_EQ(1, result->get(0).get_array()[0].get_int32());
                EXPECT_EQ(4, result->get(0).get_array()[1].get_int32());
                EXPECT_EQ(1, result->get(1).get_array()[0].get_int32());
                EXPECT_EQ(4, result->get(1).get_array()[1].get_int32());
                EXPECT_EQ(1, result->get(2).get_array()[0].get_int32());
                EXPECT_EQ(4, result->get(2).get_array()[1].get_int32());
            } else if (i == 5 && j == 1) { // array_map(x->x is null, array<const[1,4]...>)
                EXPECT_EQ(3, result->size());
                EXPECT_EQ(0, result->get(0).get_array()[0].get_int8());
                EXPECT_EQ(0, result->get(0).get_array()[1].get_int8());
                EXPECT_EQ(0, result->get(1).get_array()[0].get_int8());
                EXPECT_EQ(0, result->get(1).get_array()[1].get_int8());
                EXPECT_EQ(0, result->get(2).get_array()[0].get_int8());
                EXPECT_EQ(0, result->get(2).get_array()[1].get_int8());
            } else if (i == 5 && j == 2) { // // array_map( x->x + a, array<const[1,4]...>)
                EXPECT_EQ(3, result->size());
                EXPECT_EQ(2, result->get(0).get_array()[0].get_int32());
                EXPECT_EQ(5, result->get(0).get_array()[1].get_int32());
                EXPECT_EQ(2, result->get(1).get_array()[0].get_int32());
                EXPECT_EQ(5, result->get(1).get_array()[1].get_int32());
                EXPECT_EQ(2, result->get(2).get_array()[0].get_int32());
                EXPECT_EQ(5, result->get(2).get_array()[1].get_int32());
            } else if (i == 5 && j == 3) { // // array_map( x-> -110, array<const[1,4]...>)
                EXPECT_EQ(3, result->size());
                EXPECT_EQ(-110, result->get(0).get_array()[0].get_int32());
                EXPECT_EQ(-110, result->get(0).get_array()[1].get_int32());
                EXPECT_EQ(-110, result->get(1).get_array()[0].get_int32());
                EXPECT_EQ(-110, result->get(1).get_array()[1].get_int32());
                EXPECT_EQ(-110, result->get(2).get_array()[0].get_int32());
                EXPECT_EQ(-110, result->get(2).get_array()[1].get_int32());
            } else if (i == 6) { // array_map(x -> x || x->x is null || x -> x+a, array<const(null...)>)
                EXPECT_EQ(3, result->size());
                ASSERT_TRUE(result->is_null(0));
                ASSERT_TRUE(result->is_null(1));
                ASSERT_TRUE(result->is_null(2));
            } else if (i == 7 && (j == 0 || j == 2)) { // array_map(x -> x || x-> x+a,array<const([null]...)>)
                EXPECT_EQ(3, result->size());
                ASSERT_TRUE(result->get(0).get_array()[0].is_null());
                ASSERT_TRUE(result->get(1).get_array()[0].is_null());
                ASSERT_TRUE(result->get(2).get_array()[0].is_null());

            } else if (i == 7 && j == 1) { // array_map(x -> x is null, array<const([null]...)>)
                EXPECT_EQ(3, result->size());
                EXPECT_EQ(1, result->get(0).get_array()[0].get_int8());
                EXPECT_EQ(1, result->get(1).get_array()[0].get_int8());
                EXPECT_EQ(1, result->get(2).get_array()[0].get_int8());
            } else if (i == 7 && j == 3) { // array_map(x -> -110, array<const([null]...)>)
                EXPECT_EQ(3, result->size());
                EXPECT_EQ(-110, result->get(0).get_array()[0].get_int32());
                EXPECT_EQ(-110, result->get(1).get_array()[0].get_int32());
                EXPECT_EQ(-110, result->get(2).get_array()[0].get_int32());
            } else if (i == 8) { // array_map(x -> x || x -> x is null || x -> x+a || x -> -110, array<const([]...)>)
                EXPECT_EQ(3, result->size());
                ASSERT_TRUE(result->get(0).get_array().empty());
                ASSERT_TRUE(result->get(1).get_array().empty());
                ASSERT_TRUE(result->get(2).get_array().empty());
            }

            exprContext.close(nullptr);
        }
    }
}

} // namespace vectorized
} // namespace starrocks
