#pragma once

#include <Common/typeid_cast.h>
#include <Parsers/ASTWithElement.h>
#include <Parsers/ASTLiteral.h>
#include <Parsers/ASTQueryWithTableAndOutput.h>
#include <Parsers/ASTRenameQuery.h>
#include <Parsers/ASTIdentifier.h>
#include <Parsers/ASTRefreshStrategy.h>
#include <Parsers/ASTSelectQuery.h>
#include <Parsers/ASTSubquery.h>
#include <Parsers/ASTSelectWithUnionQuery.h>
#include <Parsers/ASTSelectIntersectExceptQuery.h>
#include <Parsers/ASTTablesInSelectQuery.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTCreateQuery.h>
#include <Parsers/DumpASTNode.h>
#include <Parsers/ASTAlterQuery.h>
#include <Interpreters/DatabaseAndTableWithAlias.h>
#include <Interpreters/IdentifierSemantic.h>
#include <Interpreters/Context.h>
#include <Interpreters/ExternalDictionariesLoader.h>
#include <Interpreters/misc.h>
#include <set>

namespace DB
{

/// Visitors consist of functions with unified interface 'void visit(Cast & x, ASTPtr & y)', there x is y, successfully cast to Cast.
/// Both types and function could have const specifiers. The second argument is used by visitor to replaces AST node (y) if needed.

/// Visits AST nodes, add default database to tables if not set. There's different logic for DDLs and selects.
class AddDefaultDatabaseVisitor
{
public:
    explicit AddDefaultDatabaseVisitor(
        ContextPtr context_,
        const String & database_name_,
        bool only_replace_current_database_function_ = false,
        bool only_replace_in_join_ = false)
        : context(context_)
        , database_name(database_name_)
        , only_replace_current_database_function(only_replace_current_database_function_)
        , only_replace_in_join(only_replace_in_join_)
    {
        if (!context->isGlobalContext())
        {
            for (const auto & [table_name, _ /* storage */] : context->getExternalTables())
            {
                external_tables.insert(table_name);
            }
        }
    }

    void visitDDL(ASTPtr & ast) const
    {
        visitDDLWithParent(nullptr, ast);
    }

    /// TODO: Add `parent` to the IAST
    void visitDDLWithParent(ASTPtr parent, ASTPtr & ast) const
    {
        visitDDLChildren(ast);

        if (!tryVisitDynamicCast<ASTAlterQuery>(parent, ast) &&
            !tryVisitDynamicCast<ASTQueryWithTableAndOutput>(parent, ast) &&
            !tryVisitDynamicCast<ASTRenameQuery>(parent, ast) &&
            !tryVisitDynamicCast<ASTFunction>(parent, ast))
        {}
    }

    void visit(ASTPtr & ast) const
    {
        if (!tryVisit<ASTSelectQuery>(ast) &&
            !tryVisit<ASTSelectWithUnionQuery>(ast) &&
            !tryVisit<ASTFunction>(ast))
            visitChildren(*ast);
    }

    void visit(ASTSelectQuery & select) const
    {
        ASTPtr unused;
        visit(select, unused);
    }

    void visit(ASTSelectWithUnionQuery & select) const
    {
        ASTPtr unused;
        visit(select, unused);
    }

    void visit(ASTColumns & columns) const
    {
        for (auto & child : columns.children)
            visit(child);
    }

    void visit(ASTRefreshStrategy & refresh) const
    {
        ASTPtr unused;
        visit(refresh, unused);
    }

private:

    ContextPtr context;

    const String database_name;
    std::set<String> external_tables;
    mutable std::unordered_set<String> with_aliases;

    bool only_replace_current_database_function = false;
    bool only_replace_in_join = false;

    void visit(ASTSelectWithUnionQuery & select, ASTPtr &) const
    {
        for (auto & child : select.list_of_selects->children)
        {
            if (child->as<ASTSelectQuery>())
                tryVisit<ASTSelectQuery>(child);
            else if (child->as<ASTSelectIntersectExceptQuery>())
                tryVisit<ASTSelectIntersectExceptQuery>(child);
        }
    }

    void visit(ASTSelectQuery & select, ASTPtr &) const
    {
        if (select.recursive_with)
            for (const auto & child : select.with()->children)
            {
                if (typeid_cast<ASTWithElement *>(child.get()))
                    with_aliases.insert(child->as<ASTWithElement>()->name);
            }

        if (select.tables())
            tryVisit<ASTTablesInSelectQuery>(select.refTables());

        visitChildren(select);
    }

    void visit(ASTSelectIntersectExceptQuery & select, ASTPtr &) const
    {
        for (auto & child : select.getListOfSelects())
        {
            if (child->as<ASTSelectQuery>())
                tryVisit<ASTSelectQuery>(child);
            else if (child->as<ASTSelectIntersectExceptQuery>())
                tryVisit<ASTSelectIntersectExceptQuery>(child);
            else if (child->as<ASTSelectWithUnionQuery>())
                tryVisit<ASTSelectWithUnionQuery>(child);
        }
    }

    void visit(ASTTablesInSelectQuery & tables, ASTPtr &) const
    {
        for (auto & child : tables.children)
            tryVisit<ASTTablesInSelectQueryElement>(child);
    }

    void visit(ASTTablesInSelectQueryElement & tables_element, ASTPtr &) const
    {
        if (only_replace_in_join && !tables_element.table_join)
            return;

        if (tables_element.table_expression)
            tryVisit<ASTTableExpression>(tables_element.table_expression);
    }

    void visit(ASTTableExpression & table_expression, ASTPtr &) const
    {
        if (table_expression.database_and_table_name)
            tryVisit<ASTTableIdentifier>(table_expression.database_and_table_name);
    }

    void visit(const ASTTableIdentifier & identifier, ASTPtr & ast) const
    {
        /// Already has database.
        if (identifier.compound())
            return;
        /// There is temporary table with such name, should not be rewritten.
        if (external_tables.contains(identifier.shortName()))
            return;
        /// This is WITH RECURSIVE alias.
        if (with_aliases.contains(identifier.name()))
            return;

        auto qualified_identifier = std::make_shared<ASTTableIdentifier>(database_name, identifier.name());
        if (!identifier.alias.empty())
            qualified_identifier->setAlias(identifier.alias);
        ast = qualified_identifier;
    }

    void visit(ASTFunction & function, ASTPtr &) const
    {
        bool is_operator_in = functionIsInOrGlobalInOperator(function.name);
        bool is_dict_get = functionIsDictGet(function.name);

        for (auto & child : function.children)
        {
            if (child.get() == function.arguments.get())
            {
                for (size_t i = 0; i < child->children.size(); ++i)
                {
                    if (is_dict_get && i == 0)
                    {
                        if (auto * identifier = child->children[i]->as<ASTIdentifier>())
                        {
                            /// Identifier already qualified
                            if (identifier->compound())
                                continue;

                            auto qualified_dictionary_name = context->getExternalDictionariesLoader().qualifyDictionaryNameWithDatabase(identifier->name(), context);
                            child->children[i] = std::make_shared<ASTIdentifier>(qualified_dictionary_name.getParts());
                        }
                        else if (auto * literal = child->children[i]->as<ASTLiteral>())
                        {
                            auto & literal_value = literal->value;

                            if (literal_value.getType() != Field::Types::String)
                                continue;

                            auto dictionary_name = literal_value.safeGet<String>();
                            auto qualified_dictionary_name = context->getExternalDictionariesLoader().qualifyDictionaryNameWithDatabase(dictionary_name, context);
                            literal_value = qualified_dictionary_name.getFullName();
                        }
                    }
                    else if (is_operator_in && i == 1)
                    {
                        /// XXX: for some unknown reason this place assumes that argument can't be an alias,
                        ///      like in the similar code in `MarkTableIdentifierVisitor`.
                        if (auto * identifier = child->children[i]->as<ASTIdentifier>())
                        {
                            /// If identifier is broken then we can do nothing and get an exception
                            auto maybe_table_identifier = identifier->createTable();
                            if (maybe_table_identifier)
                                child->children[i] = maybe_table_identifier;
                        }

                        /// Second argument of the "in" function (or similar) may be a table name or a subselect.
                        /// Rewrite the table name or descend into subselect.
                        if (!tryVisit<ASTTableIdentifier>(child->children[i]))
                            visit(child->children[i]);
                    }
                    else
                    {
                        visit(child->children[i]);
                    }
                }
            }
            else
            {
                visit(child);
            }
        }
    }

    void visit(ASTRefreshStrategy & refresh, ASTPtr &) const
    {
        if (refresh.dependencies)
            for (auto & table : refresh.dependencies->children)
                tryVisit<ASTTableIdentifier>(table);
    }

    void visitChildren(IAST & ast) const
    {
        for (auto & child : ast.children)
            visit(child);
    }

    template <typename T>
    bool tryVisit(ASTPtr & ast) const
    {
        if (T * t = typeid_cast<T *>(ast.get()))
        {
            visit(*t, ast);
            return true;
        }
        return false;
    }


    void visitDDL(ASTPtr & /* parent */, ASTQueryWithTableAndOutput & node, ASTPtr &) const
    {
        if (only_replace_current_database_function)
            return;

        if (!node.database)
            node.setDatabase(database_name);
    }

    void visitDDL(ASTPtr & /* parent */, ASTRenameQuery & node, ASTPtr &) const
    {
        if (only_replace_current_database_function)
            return;

        node.setDatabaseIfNotExists(database_name);
    }

    void visitDDL(ASTPtr & /* parent */, ASTAlterQuery & node, ASTPtr &) const
    {
        if (only_replace_current_database_function)
            return;

        if (!node.database)
            node.setDatabase(database_name);

        for (const auto & child : node.command_list->children)
        {
            auto * command_ast = child->as<ASTAlterCommand>();
            if (command_ast->from_database.empty())
                command_ast->from_database = database_name;
            if (command_ast->to_database.empty())
                command_ast->to_database = database_name;
        }
    }

    void visitDDL(ASTPtr & parent, ASTFunction & function, ASTPtr & node) const
    {
        if (function.name == "currentDatabase")
        {
            /// The `updatePointerToChild` function replaces the old address with the new one without access, so it is safe to invalidate it in place.
            /// However, just for safety, let's store the old node for a little longer.
            ASTPtr old_node = node;
            node = std::make_shared<ASTLiteral>(database_name);

            if (parent)
            {
                parent->updatePointerToChild(old_node.get(), node.get());
            }
        }
    }

    void visitDDLChildren(ASTPtr & ast) const
    {
        for (auto & child : ast->children)
            visitDDLWithParent(ast, child);
    }

    template <typename T>
    bool tryVisitDynamicCast(ASTPtr & parent, ASTPtr & ast) const
    {
        if (T * t = dynamic_cast<T *>(ast.get()))
        {
            visitDDL(parent, *t, ast);
            return true;
        }
        return false;
    }
};

}
