#include <google/protobuf/text_format.h>
#include <google/protobuf/util/message_differencer.h>
#include <gtest/gtest.h>
#include <pypa/ast/ast.hh>

#include "src/carnot/compiler/ir_nodes.h"
#include "src/carnot/compiler/ir_test_utils.h"
#include "src/carnot/compiler/metadata_handler.h"
#include "src/carnot/compiler/pattern_match.h"
#include "src/carnot/compiler/test_utils.h"
#include "src/common/testing/protobuf.h"
#include "src/table_store/table_store.h"

namespace pl {
namespace carnot {
namespace compiler {
using ::pl::testing::proto::EqualsProto;
using ::testing::ElementsAre;

TEST(IRTypes, types_enum_test) {
  // Quick test to make sure the enums test is inline with the type strings.
  EXPECT_EQ(static_cast<int64_t>(IRNodeType::number_of_types),
            sizeof(kIRNodeStrings) / sizeof(*kIRNodeStrings));
}

/**
 * Creates IR Graph that is the following query compiled
 *
 * `From(table="tableName", select=["testCol"]).Range("-2m")`
 */

TEST(IRTest, check_connection) {
  auto ast = MakeTestAstPtr();
  auto ig = std::make_shared<IR>();
  auto src = ig->MakeNode<MemorySourceIR>().ValueOrDie();
  auto range = ig->MakeNode<RangeIR>().ValueOrDie();
  auto start_rng_str = ig->MakeNode<IntIR>().ValueOrDie();
  auto stop_rng_str = ig->MakeNode<IntIR>().ValueOrDie();
  auto table_str_node = ig->MakeNode<StringIR>().ValueOrDie();
  auto select_col = ig->MakeNode<StringIR>().ValueOrDie();
  auto select_list = ig->MakeNode<ListIR>().ValueOrDie();
  EXPECT_OK(start_rng_str->Init(0, ast));
  EXPECT_OK(stop_rng_str->Init(10, ast));
  std::string table_str = "tableName";
  EXPECT_OK(table_str_node->Init(table_str, ast));
  EXPECT_OK(select_col->Init("testCol", ast));
  EXPECT_OK(select_list->Init(ast, {select_col}));
  ArgMap memsrc_argmap({{"table", table_str_node}, {"select", select_list}});
  EXPECT_OK(src->Init(nullptr, memsrc_argmap, ast));
  EXPECT_OK(range->Init(src, start_rng_str, stop_rng_str, ast));
  EXPECT_EQ(range->parents()[0], src);
  EXPECT_EQ(range->start_repr(), start_rng_str);
  EXPECT_EQ(range->stop_repr(), stop_rng_str);
  EXPECT_EQ(src->table_name(), table_str);
  EXPECT_THAT(src->column_names(), ElementsAre("testCol"));
  EXPECT_EQ(select_list->children()[0], select_col);
  EXPECT_EQ(select_col->str(), "testCol");
  VerifyGraphConnections(ig.get());
}  // namespace compiler

TEST(IRWalker, basic_tests) {
  // Construct example IR Graph.
  auto graph = std::make_shared<IR>();

  // Create nodes.
  auto src = graph->MakeNode<MemorySourceIR>().ValueOrDie();
  auto select_list = graph->MakeNode<ListIR>().ValueOrDie();
  auto map = graph->MakeNode<MapIR>().ValueOrDie();
  auto agg = graph->MakeNode<BlockingAggIR>().ValueOrDie();
  auto sink = graph->MakeNode<MemorySinkIR>().ValueOrDie();

  // Add dependencies.
  EXPECT_OK(graph->AddEdge(src, select_list));
  EXPECT_OK(graph->AddEdge(src, map));
  EXPECT_OK(graph->AddEdge(map, agg));
  EXPECT_OK(graph->AddEdge(agg, sink));

  std::vector<int64_t> call_order;
  auto s = IRWalker()
               .OnMemorySink([&](auto& mem_sink) {
                 call_order.push_back(mem_sink.id());
                 return Status::OK();
               })
               .OnMemorySource([&](auto& mem_src) {
                 call_order.push_back(mem_src.id());
                 return Status::OK();
               })
               .OnMap([&](auto& map) {
                 call_order.push_back(map.id());
                 return Status::OK();
               })
               .OnBlockingAggregate([&](auto& agg) {
                 call_order.push_back(agg.id());
                 return Status::OK();
               })
               .Walk(*graph);
  EXPECT_OK(s);
  EXPECT_THAT(call_order, ElementsAre(0, 2, 3, 4));
}

const char* kExpectedMemSrcPb = R"(
  op_type: MEMORY_SOURCE_OPERATOR
  mem_source_op {
    name: "test_table"
    column_idxs: 0
    column_idxs: 2
    column_names: "cpu0"
    column_names: "cpu1"
    column_types: INT64
    column_types: FLOAT64
    start_time: {
      value: 10
    }
    stop_time: {
      value: 20
    }
  }
)";

TEST(ToProto, memory_source_ir) {
  auto ast = MakeTestAstPtr();
  auto graph = std::make_shared<IR>();

  auto mem_src = graph->MakeNode<MemorySourceIR>().ValueOrDie();
  auto select_list = graph->MakeNode<ListIR>().ValueOrDie();
  auto table_node = graph->MakeNode<StringIR>().ValueOrDie();
  EXPECT_OK(table_node->Init("test_table", ast));
  ArgMap memsrc_argmap({{"table", table_node}, {"select", select_list}});
  EXPECT_OK(mem_src->Init(nullptr, memsrc_argmap, ast));

  auto col_1 = graph->MakeNode<ColumnIR>().ValueOrDie();
  EXPECT_OK(col_1->Init("cpu0", /*parent_op_idx*/ 0, ast));
  col_1->ResolveColumn(0, types::DataType::INT64);

  auto col_2 = graph->MakeNode<ColumnIR>().ValueOrDie();
  EXPECT_OK(col_2->Init("cpu1", /*parent_op_idx*/ 0, ast));
  col_2->ResolveColumn(2, types::DataType::FLOAT64);

  mem_src->SetColumns(std::vector<ColumnIR*>({col_1, col_2}));
  mem_src->SetTime(10, 20);

  planpb::Operator pb;
  EXPECT_OK(mem_src->ToProto(&pb));

  planpb::Operator expected_pb;
  ASSERT_TRUE(google::protobuf::TextFormat::MergeFromString(kExpectedMemSrcPb, &expected_pb));
  EXPECT_TRUE(google::protobuf::util::MessageDifferencer::Equals(expected_pb, pb));
}

const char* kExpectedMemSinkPb = R"(
  op_type: MEMORY_SINK_OPERATOR
  mem_sink_op {
    name: "output_table"
    column_names: "output1"
    column_names: "output2"
    column_types: INT64
    column_types: FLOAT64
  }
)";

TEST(ToProto, memory_sink_ir) {
  auto ast = MakeTestAstPtr();
  auto graph = std::make_shared<IR>();

  auto mem_sink = graph->MakeNode<MemorySinkIR>().ValueOrDie();
  auto mem_source = graph->MakeNode<MemorySourceIR>().ValueOrDie();
  auto name_ir = graph->MakeNode<StringIR>().ValueOrDie();

  auto rel = table_store::schema::Relation(
      std::vector<types::DataType>({types::DataType::INT64, types::DataType::FLOAT64}),
      std::vector<std::string>({"output1", "output2"}));
  EXPECT_OK(mem_sink->SetRelation(rel));
  EXPECT_OK(name_ir->Init("output_table", ast));
  ArgMap amap({{"name", name_ir}});
  EXPECT_OK(mem_sink->Init(mem_source, amap, ast));

  planpb::Operator pb;
  EXPECT_OK(mem_sink->ToProto(&pb));

  planpb::Operator expected_pb;
  ASSERT_TRUE(google::protobuf::TextFormat::MergeFromString(kExpectedMemSinkPb, &expected_pb));
  EXPECT_TRUE(google::protobuf::util::MessageDifferencer::Equals(expected_pb, pb));
}

const char* kExpectedMapPb = R"(
  op_type: MAP_OPERATOR
  map_op {
    column_names: "col_name"
    expressions {
      func {
        id: 1
        name: "pl.add"
        args {
          constant {
            data_type: INT64
            int64_value: 10
          }
        }
        args {
          column {
            node: 0
            index: 4
          }
        }
      }
    }
  }
)";

TEST(ToProto, map_ir) {
  auto ast = MakeTestAstPtr();
  auto graph = std::make_shared<IR>();
  auto mem_src = graph->MakeNode<MemorySourceIR>().ValueOrDie();
  auto map = graph->MakeNode<MapIR>().ValueOrDie();
  auto constant = graph->MakeNode<IntIR>().ValueOrDie();
  EXPECT_OK(constant->Init(10, ast));
  auto col = graph->MakeNode<ColumnIR>().ValueOrDie();
  EXPECT_OK(col->Init("col_name", /*parent_op_idx*/ 0, ast));
  col->ResolveColumn(4, types::INT64);
  auto func = graph->MakeNode<FuncIR>().ValueOrDie();
  auto lambda = graph->MakeNode<LambdaIR>().ValueOrDie();
  EXPECT_OK(func->Init({FuncIR::Opcode::add, "+", "add"}, ASTWalker::kRunTimeFuncPrefix,
                       std::vector<ExpressionIR*>({constant, col}), false /* compile_time */, ast));
  func->set_func_id(1);
  EXPECT_OK(lambda->Init({"col_name"}, {{"col_name", func}}, ast));
  ArgMap amap({{"fn", lambda}});
  EXPECT_OK(map->Init(mem_src, amap, ast));

  planpb::Operator pb;
  EXPECT_OK(map->ToProto(&pb));

  planpb::Operator expected_pb;
  ASSERT_TRUE(google::protobuf::TextFormat::MergeFromString(kExpectedMapPb, &expected_pb));
  EXPECT_TRUE(google::protobuf::util::MessageDifferencer::Equals(expected_pb, pb));
}

const char* kExpectedAggPb = R"(
  op_type: AGGREGATE_OPERATOR
  agg_op {
    windowed: false
    values {
      name: "pl.mean"
      id: 0
      args {
        constant {
          data_type: INT64
          int64_value: 10
        }
      }
      args {
        column {
          node: 0
          index: 4
        }
      }
    }
    groups {
      node: 0
      index: 1
    }
    group_names: "group1"
    value_names: "mean"
  }
)";

TEST(ToProto, agg_ir) {
  auto ast = MakeTestAstPtr();
  auto graph = std::make_shared<IR>();
  auto mem_src = graph->MakeNode<MemorySourceIR>().ValueOrDie();
  auto agg = graph->MakeNode<BlockingAggIR>().ValueOrDie();
  auto constant = graph->MakeNode<IntIR>().ValueOrDie();
  EXPECT_OK(constant->Init(10, ast));
  auto col = graph->MakeNode<ColumnIR>().ValueOrDie();
  EXPECT_OK(col->Init("column", /*parent_op_idx*/ 0, ast));
  col->ResolveColumn(4, types::INT64);

  auto agg_func_lambda = graph->MakeNode<LambdaIR>().ValueOrDie();
  auto agg_func = graph->MakeNode<FuncIR>().ValueOrDie();
  EXPECT_OK(agg_func->Init({FuncIR::Opcode::non_op, "", "mean"}, ASTWalker::kRunTimeFuncPrefix,
                           std::vector<ExpressionIR*>({constant, col}), false /* compile_time */,
                           ast));
  EXPECT_OK(agg_func_lambda->Init({"meaned_column"}, {{"mean", agg_func}}, ast));

  auto by_func_lambda = graph->MakeNode<LambdaIR>().ValueOrDie();
  auto group1 = graph->MakeNode<ColumnIR>().ValueOrDie();
  EXPECT_OK(group1->Init("group1", /*parent_op_idx*/ 0, ast));
  group1->ResolveColumn(1, types::INT64);
  EXPECT_OK(by_func_lambda->Init({"group1"}, group1, ast));
  ArgMap amap({{"by", by_func_lambda}, {"fn", agg_func_lambda}});

  ASSERT_OK(agg->Init(mem_src, amap, ast));
  ColExpressionVector exprs;
  exprs.push_back(ColumnExpression({"value1", agg_func}));

  planpb::Operator pb;
  ASSERT_OK(agg->ToProto(&pb));

  planpb::Operator expected_pb;
  ASSERT_TRUE(google::protobuf::TextFormat::MergeFromString(kExpectedAggPb, &expected_pb));
  EXPECT_TRUE(google::protobuf::util::MessageDifferencer::Equals(expected_pb, pb));
  EXPECT_THAT(pb, EqualsProto(kExpectedAggPb));
}

class MetadataTests : public ::testing::Test {
 protected:
  void SetUp() override {
    ast = MakeTestAstPtr();
    graph = std::make_shared<IR>();
    md_handler = MetadataHandler::Create();
  }
  MemorySourceIR* MakeMemSource() { return graph->MakeNode<MemorySourceIR>().ValueOrDie(); }
  pypa::AstPtr ast;
  std::shared_ptr<IR> graph;
  std::unique_ptr<MetadataHandler> md_handler;
};

TEST_F(MetadataTests, metadata_resolver) {
  MetadataResolverIR* metadata_resolver = graph->MakeNode<MetadataResolverIR>().ValueOrDie();
  EXPECT_OK(metadata_resolver->Init(MakeMemSource(), {{}}, ast));
  MetadataProperty* md_property = md_handler->GetProperty("pod_name").ValueOrDie();
  EXPECT_FALSE(metadata_resolver->HasMetadataColumn("pod_name"));
  EXPECT_OK(metadata_resolver->AddMetadata(md_property));
  EXPECT_TRUE(metadata_resolver->HasMetadataColumn("pod_name"));
  EXPECT_EQ(metadata_resolver->metadata_columns().size(), 1);
  EXPECT_EQ(metadata_resolver->metadata_columns().find("pod_name")->second, md_property);
}

TEST_F(MetadataTests, metadata_ir) {
  MetadataResolverIR* metadata_resolver = graph->MakeNode<MetadataResolverIR>().ValueOrDie();
  MetadataIR* metadata_ir = graph->MakeNode<MetadataIR>().ValueOrDie();
  EXPECT_OK(metadata_ir->Init("pod_name", /*parent_op_idx*/ 0, ast));
  EXPECT_TRUE(metadata_ir->IsColumn());
  EXPECT_FALSE(metadata_ir->HasMetadataResolver());
  EXPECT_EQ(metadata_ir->name(), "pod_name");
  EXPECT_OK(metadata_resolver->Init(MakeMemSource(), {{}}, ast));
  auto property = std::make_unique<NameMetadataProperty>(
      MetadataType::POD_NAME, std::vector<MetadataType>({MetadataType::POD_ID}));
  EXPECT_OK(metadata_ir->ResolveMetadataColumn(metadata_resolver, property.get()));
  EXPECT_TRUE(metadata_ir->HasMetadataResolver());
}

class OperatorTests : public ::testing::Test {
 protected:
  void SetUp() override {
    ast = MakeTestAstPtr();
    graph = std::make_shared<IR>();
  }
  MemorySourceIR* MakeMemSource() { return graph->MakeNode<MemorySourceIR>().ValueOrDie(); }
  MapIR* MakeMap(OperatorIR* parent, const ColExpressionVector& col_map) {
    MapIR* map = graph->MakeNode<MapIR>().ConsumeValueOrDie();
    LambdaIR* lambda = graph->MakeNode<LambdaIR>().ConsumeValueOrDie();
    PL_CHECK_OK(lambda->Init({}, col_map, ast));
    PL_CHECK_OK(map->Init(parent, {{"fn", lambda}}, ast));
    return map;
  }
  MemorySinkIR* MakeMemSink(OperatorIR* parent, std::string name) {
    auto sink = graph->MakeNode<MemorySinkIR>().ValueOrDie();
    PL_CHECK_OK(sink->Init(parent, {{"name", MakeString(name)}}, ast));
    return sink;
  }
  FilterIR* MakeFilter(OperatorIR* parent, ExpressionIR* filter_expr) {
    auto filter_func_lambda = graph->MakeNode<LambdaIR>().ValueOrDie();
    EXPECT_OK(filter_func_lambda->Init({}, filter_expr, ast));

    FilterIR* filter = graph->MakeNode<FilterIR>().ValueOrDie();
    ArgMap amap({{"fn", filter_func_lambda}});
    EXPECT_OK(filter->Init(parent, amap, ast));
    return filter;
  }
  LimitIR* MakeLimit(OperatorIR* parent, int64_t limit_value) {
    LimitIR* limit = graph->MakeNode<LimitIR>().ValueOrDie();
    ArgMap amap({{"rows", MakeInt(limit_value)}});
    EXPECT_OK(limit->Init(parent, amap, ast));
    return limit;
  }
  BlockingAggIR* MakeBlockingAgg(OperatorIR* parent, const std::vector<ColumnIR*>& columns,
                                 const ColExpressionVector& col_agg) {
    BlockingAggIR* agg = graph->MakeNode<BlockingAggIR>().ConsumeValueOrDie();
    LambdaIR* fn_lambda = graph->MakeNode<LambdaIR>().ConsumeValueOrDie();
    PL_CHECK_OK(fn_lambda->Init({}, col_agg, ast));
    ListIR* list_ir = graph->MakeNode<ListIR>().ConsumeValueOrDie();
    std::vector<ExpressionIR*> exprs;
    for (auto c : columns) {
      exprs.push_back(c);
    }
    PL_CHECK_OK(list_ir->Init(ast, exprs));
    LambdaIR* by_lambda = graph->MakeNode<LambdaIR>().ConsumeValueOrDie();
    PL_CHECK_OK(by_lambda->Init({}, list_ir, ast));
    PL_CHECK_OK(agg->Init(parent, {{"by", by_lambda}, {"fn", fn_lambda}}, ast));
    return agg;
  }

  ColumnIR* MakeColumn(const std::string& name, int64_t parent_op_idx) {
    ColumnIR* column = graph->MakeNode<ColumnIR>().ValueOrDie();
    PL_CHECK_OK(column->Init(name, parent_op_idx, ast));
    return column;
  }
  StringIR* MakeString(std::string val) {
    auto str_ir = graph->MakeNode<StringIR>().ValueOrDie();
    EXPECT_OK(str_ir->Init(val, ast));
    return str_ir;
  }
  IntIR* MakeInt(int64_t val) {
    auto int_ir = graph->MakeNode<IntIR>().ValueOrDie();
    EXPECT_OK(int_ir->Init(val, ast));
    return int_ir;
  }
  FuncIR* MakeAddFunc(ExpressionIR* left, ExpressionIR* right) {
    FuncIR* func = graph->MakeNode<FuncIR>().ValueOrDie();
    PL_CHECK_OK(func->Init({FuncIR::Opcode::add, "+", "add"}, ASTWalker::kRunTimeFuncPrefix,
                           std::vector<ExpressionIR*>({left, right}), false /* compile_time */,
                           ast));
    return func;
  }
  FuncIR* MakeEqualsFunc(ExpressionIR* left, ExpressionIR* right) {
    FuncIR* func = graph->MakeNode<FuncIR>().ValueOrDie();
    PL_CHECK_OK(func->Init({FuncIR::Opcode::eq, "==", "equals"}, ASTWalker::kRunTimeFuncPrefix,
                           std::vector<ExpressionIR*>({left, right}), false /* compile_time */,
                           ast));
    return func;
  }
  MetadataIR* MakeMetadataIR(const std::string& name, int64_t parent_op_idx) {
    MetadataIR* metadata = graph->MakeNode<MetadataIR>().ValueOrDie();
    PL_CHECK_OK(metadata->Init(name, parent_op_idx, ast));
    return metadata;
  }
  MetadataLiteralIR* MakeMetadataLiteral(DataIR* data_ir) {
    MetadataLiteralIR* metadata_literal = graph->MakeNode<MetadataLiteralIR>().ValueOrDie();
    PL_CHECK_OK(metadata_literal->Init(data_ir, ast));
    return metadata_literal;
  }
  FuncIR* MakeMeanFunc(ExpressionIR* value) {
    FuncIR* func = graph->MakeNode<FuncIR>().ValueOrDie();
    PL_CHECK_OK(func->Init({FuncIR::Opcode::non_op, "", "mean"}, ASTWalker::kRunTimeFuncPrefix,
                           std::vector<ExpressionIR*>({value}), false /* compile_time */, ast));
    return func;
  }

  pypa::AstPtr ast;
  std::shared_ptr<IR> graph;
};

// Swapping a parent should make sure that all columns are passed over correclt.
TEST_F(OperatorTests, swap_parent) {
  MemorySourceIR* mem_source = MakeMemSource();
  ColumnIR* col1 = MakeColumn("test1", /*parent_op_idx*/ 0);
  ColumnIR* col2 = MakeColumn("test2", /*parent_op_idx*/ 0);
  ColumnIR* col3 = MakeColumn("test3", /*parent_op_idx*/ 0);
  FuncIR* add_func = MakeAddFunc(col3, MakeInt(3));
  MapIR* child_map = MakeMap(mem_source, {{"out11", col1}, {"out2", col2}, {"out3", add_func}});
  EXPECT_EQ(col1->ReferenceID().ConsumeValueOrDie(), mem_source->id());
  EXPECT_EQ(col2->ReferenceID().ConsumeValueOrDie(), mem_source->id());
  EXPECT_EQ(col3->ReferenceID().ConsumeValueOrDie(), mem_source->id());

  // Insert a map as if we are copying from the parent. These columns are distinact from col1-3.
  MapIR* parent_map = MakeMap(mem_source, {{"test1", MakeColumn("test1", /*parent_op_idx*/ 0)},
                                           {"test2", MakeColumn("test2", /*parent_op_idx*/ 0)},
                                           {"test3", MakeColumn("test3", /*parent_op_idx*/ 0)}});

  EXPECT_NE(parent_map->id(), child_map->id());  // Sanity check.
  // Now swap the parent, and expect the children to point to the new parent.
  EXPECT_OK(child_map->ReplaceParent(mem_source, parent_map));
  EXPECT_EQ(col1->ReferenceID().ConsumeValueOrDie(), parent_map->id());
  EXPECT_EQ(col2->ReferenceID().ConsumeValueOrDie(), parent_map->id());
  EXPECT_EQ(col3->ReferenceID().ConsumeValueOrDie(), parent_map->id());
}
class CloneTests : public OperatorTests {
 protected:
  void CompareClonedColumn(ColumnIR* new_ir, ColumnIR* old_ir, const std::string& failure_string) {
    if (new_ir->graph_ptr() != old_ir->graph_ptr()) {
      EXPECT_NE(new_ir->ContainingOperator().ConsumeValueOrDie()->graph_ptr(),
                old_ir->ContainingOperator().ConsumeValueOrDie()->graph_ptr())
          << absl::Substitute(
                 "'$1' and '$2' should have container ops that are in different graphs. $0.",
                 failure_string, new_ir->DebugString(), old_ir->DebugString());
    }
    EXPECT_EQ(new_ir->ReferencedOperator().ConsumeValueOrDie()->id(),
              old_ir->ReferencedOperator().ConsumeValueOrDie()->id())
        << failure_string;
    EXPECT_EQ(new_ir->col_name(), old_ir->col_name()) << failure_string;
  }
  void CompareClonedMap(MapIR* new_ir, MapIR* old_ir, const std::string& failure_string) {
    std::vector<ColumnExpression> new_col_exprs = new_ir->col_exprs();
    std::vector<ColumnExpression> old_col_exprs = old_ir->col_exprs();
    ASSERT_EQ(new_col_exprs.size(), old_col_exprs.size()) << failure_string;
    for (size_t i = 0; i < new_col_exprs.size(); ++i) {
      ColumnExpression new_expr = new_col_exprs[i];
      ColumnExpression old_expr = old_col_exprs[i];
      EXPECT_EQ(new_expr.name, old_expr.name) << failure_string;
      EXPECT_EQ(new_expr.node->type_string(), old_expr.node->type_string()) << failure_string;
      EXPECT_EQ(new_expr.node->id(), old_expr.node->id()) << failure_string;
    }
  }

  void CompareClonedBlockingAgg(BlockingAggIR* new_ir, BlockingAggIR* old_ir,
                                const std::string& failure_string) {
    std::vector<ColumnExpression> new_col_exprs = new_ir->aggregate_expressions();
    std::vector<ColumnExpression> old_col_exprs = old_ir->aggregate_expressions();
    ASSERT_EQ(new_col_exprs.size(), old_col_exprs.size()) << failure_string;
    for (size_t i = 0; i < new_col_exprs.size(); ++i) {
      ColumnExpression new_expr = new_col_exprs[i];
      ColumnExpression old_expr = old_col_exprs[i];
      EXPECT_EQ(new_expr.name, old_expr.name) << failure_string;
      EXPECT_EQ(new_expr.node->type_string(), old_expr.node->type_string()) << failure_string;
      EXPECT_EQ(new_expr.node->id(), old_expr.node->id()) << failure_string;
    }

    std::vector<ColumnIR*> new_groups = new_ir->groups();
    std::vector<ColumnIR*> old_groups = old_ir->groups();
    ASSERT_EQ(new_groups.size(), old_groups.size()) << failure_string;
    for (size_t i = 0; i < new_groups.size(); ++i) {
      CompareClonedColumn(new_groups[i], old_groups[i], failure_string);
    }
  }
  void CompareClonedMetadata(MetadataIR* new_ir, MetadataIR* old_ir,
                             const std::string& err_string) {
    CompareClonedColumn(new_ir, old_ir, err_string);
    EXPECT_EQ(new_ir->property(), old_ir->property())
        << absl::Substitute("Expected Metadata properties to be the same. Got $1 vs $2. $0.",
                            err_string, new_ir->property()->name(), old_ir->property()->name());
    EXPECT_EQ(new_ir->name(), old_ir->name())
        << absl::Substitute("Expected Metadata names to be the same. Got $1 vs $2. $0.", err_string,
                            new_ir->name(), old_ir->name());
  }
  void CompareClonedMetadataLiteral(MetadataLiteralIR* new_ir, MetadataLiteralIR* old_ir,
                                    const std::string& err_string) {
    EXPECT_EQ(new_ir->literal_type(), old_ir->literal_type()) << err_string;
    EXPECT_EQ(new_ir->literal()->id(), old_ir->literal()->id()) << err_string;
  }
  void CompareClonedMemorySource(MemorySourceIR* new_ir, MemorySourceIR* old_ir,
                                 const std::string& err_string) {
    EXPECT_EQ(new_ir->table_name(), old_ir->table_name()) << err_string;
    EXPECT_EQ(new_ir->IsTimeSet(), old_ir->IsTimeSet()) << err_string;
    EXPECT_EQ(new_ir->time_start_ns(), old_ir->time_start_ns()) << err_string;
    EXPECT_EQ(new_ir->time_stop_ns(), old_ir->time_stop_ns()) << err_string;
    EXPECT_EQ(new_ir->column_names(), old_ir->column_names()) << err_string;
    EXPECT_EQ(new_ir->columns_set(), old_ir->columns_set()) << err_string;
  }
  void CompareClonedMemorySink(MemorySinkIR* new_ir, MemorySinkIR* old_ir,
                               const std::string& err_string) {
    EXPECT_EQ(new_ir->name(), old_ir->name()) << err_string;
    EXPECT_EQ(new_ir->name_set(), old_ir->name_set()) << err_string;
  }
  void CompareClonedFilter(FilterIR* new_ir, FilterIR* old_ir, const std::string& err_string) {
    CompareClonedExpression(new_ir->filter_expr(), old_ir->filter_expr(), err_string);
  }
  void CompareClonedLimit(LimitIR* new_ir, LimitIR* old_ir, const std::string& err_string) {
    EXPECT_EQ(new_ir->limit_value(), old_ir->limit_value()) << err_string;
    EXPECT_EQ(new_ir->limit_value_set(), old_ir->limit_value_set()) << err_string;
  }
  void CompareClonedFunc(FuncIR* new_ir, FuncIR* old_ir, const std::string& err_string) {
    EXPECT_EQ(new_ir->func_name(), old_ir->func_name()) << err_string;
    EXPECT_EQ(new_ir->op().op_code, old_ir->op().op_code) << err_string;
    EXPECT_EQ(new_ir->op().python_op, old_ir->op().python_op) << err_string;
    EXPECT_EQ(new_ir->op().carnot_op_name, old_ir->op().carnot_op_name) << err_string;
    EXPECT_EQ(new_ir->func_id(), old_ir->func_id()) << err_string;
    EXPECT_EQ(new_ir->is_compile_time(), old_ir->is_compile_time()) << err_string;
    EXPECT_EQ(new_ir->IsDataTypeEvaluated(), old_ir->IsDataTypeEvaluated()) << err_string;
    EXPECT_EQ(new_ir->EvaluatedDataType(), old_ir->EvaluatedDataType()) << err_string;

    std::vector<ExpressionIR*> new_args = new_ir->args();
    std::vector<ExpressionIR*> old_args = old_ir->args();
    ASSERT_EQ(new_args.size(), old_args.size()) << err_string;
    for (size_t i = 0; i < new_args.size(); ++i) {
      CompareClonedExpression(new_args[i], old_args[i], err_string);
    }
  }

  void CompareClonedExpression(ExpressionIR* new_ir, ExpressionIR* old_ir,
                               const std::string& err_string) {
    ASSERT_NE(new_ir, nullptr);
    ASSERT_NE(old_ir, nullptr);
    if (Match(new_ir, ColumnNode())) {
      CompareClonedColumn(static_cast<ColumnIR*>(new_ir), static_cast<ColumnIR*>(old_ir),
                          err_string);
    } else if (Match(new_ir, Func())) {
      CompareClonedFunc(static_cast<FuncIR*>(new_ir), static_cast<FuncIR*>(old_ir), err_string);

    } else if (Match(new_ir, MetadataLiteral())) {
      CompareClonedMetadataLiteral(static_cast<MetadataLiteralIR*>(new_ir),
                                   static_cast<MetadataLiteralIR*>(old_ir), err_string);
    } else if (Match(new_ir, Metadata())) {
      CompareClonedMetadata(static_cast<MetadataIR*>(new_ir), static_cast<MetadataIR*>(old_ir),
                            err_string);
    }
  }

  void CompareClonedOperator(OperatorIR* new_ir, OperatorIR* old_ir,
                             const std::string& err_string) {
    std::string new_err_string =
        absl::Substitute("$0. In $1 Operator.", err_string, new_ir->type_string());
    if (Match(new_ir, MemorySource())) {
      CompareClonedMemorySource(static_cast<MemorySourceIR*>(new_ir),
                                static_cast<MemorySourceIR*>(old_ir), new_err_string);
    } else if (Match(new_ir, MemorySink())) {
      CompareClonedMemorySink(static_cast<MemorySinkIR*>(new_ir),
                              static_cast<MemorySinkIR*>(old_ir), new_err_string);
    } else if (Match(new_ir, Filter())) {
      CompareClonedFilter(static_cast<FilterIR*>(new_ir), static_cast<FilterIR*>(old_ir),
                          new_err_string);
    } else if (Match(new_ir, Limit())) {
      CompareClonedLimit(static_cast<LimitIR*>(new_ir), static_cast<LimitIR*>(old_ir),
                         new_err_string);
    } else if (Match(new_ir, Map())) {
      CompareClonedMap(static_cast<MapIR*>(new_ir), static_cast<MapIR*>(old_ir), new_err_string);
    } else if (Match(new_ir, BlockingAgg())) {
      CompareClonedBlockingAgg(static_cast<BlockingAggIR*>(new_ir),
                               static_cast<BlockingAggIR*>(old_ir), new_err_string);
    }
  }
  void CompareClonedNodes(IRNode* new_ir, IRNode* old_ir, const std::string& err_string) {
    EXPECT_NE(old_ir, new_ir) << err_string;
    ASSERT_EQ(old_ir->type_string(), new_ir->type_string()) << err_string;
    if (Match(new_ir, Expression())) {
      CompareClonedExpression(static_cast<ExpressionIR*>(new_ir),
                              static_cast<ExpressionIR*>(old_ir), err_string);
    } else if (Match(new_ir, Operator())) {
      CompareClonedOperator(static_cast<OperatorIR*>(new_ir), static_cast<OperatorIR*>(old_ir),
                            err_string);
    }
  }
};

TEST_F(CloneTests, simple_clone) {
  auto mem_source = MakeMemSource();
  ColumnIR* col1 = MakeColumn("test1", 0);
  ColumnIR* col2 = MakeColumn("test2", 0);
  ColumnIR* col3 = MakeColumn("test3", 0);
  FuncIR* add_func = MakeAddFunc(col3, MakeInt(3));
  MapIR* map = MakeMap(mem_source, {{"out1", col1}, {"out2", col2}, {"out3", add_func}});
  MakeMemSink(map, "out");

  auto out = graph->Clone();
  EXPECT_OK(out.status());
  std::unique_ptr<IR> cloned_ir = out.ConsumeValueOrDie();

  ASSERT_EQ(graph->dag().TopologicalSort(), cloned_ir->dag().TopologicalSort());

  // Make sure that all of the columns are now part of the new graph.
  for (int64_t i : cloned_ir->dag().TopologicalSort()) {
    CompareClonedNodes(cloned_ir->Get(i), graph->Get(i), absl::Substitute("For index $0", i));
  }
}

TEST_F(CloneTests, all_op_clone) {
  auto mem_source = MakeMemSource();
  auto filter =
      MakeFilter(mem_source, MakeEqualsFunc(MakeMetadataIR("service", 0),
                                            MakeMetadataLiteral(MakeString("pl/test_service"))));
  auto limit = MakeLimit(filter, 10);

  auto agg = MakeBlockingAgg(limit, {MakeMetadataIR("service", 0)},
                             {{"mean", MakeMeanFunc(MakeColumn("equals_column", 0))}});
  auto map = MakeMap(agg, {{"mean_deux", MakeAddFunc(MakeColumn("mean", 0), MakeInt(3))},
                           {"mean", MakeColumn("mean", 0)}});
  MakeMemSink(map, "sup");
  auto out = graph->Clone();
  EXPECT_OK(out.status());
  std::unique_ptr<IR> cloned_ir = out.ConsumeValueOrDie();

  ASSERT_EQ(graph->dag().TopologicalSort(), cloned_ir->dag().TopologicalSort());

  // Make sure that all of the columns are now part of the new graph.
  for (int64_t i : cloned_ir->dag().TopologicalSort()) {
    CompareClonedNodes(cloned_ir->Get(i), graph->Get(i), absl::Substitute("For index $0", i));
  }
}

}  // namespace compiler
}  // namespace carnot
}  // namespace pl
