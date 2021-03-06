/**
 *   Copyright (C) 2010, 2011 Object Modeling Designs
 *
 *   Distributed under the Boost Software License, Version 1.0. (See accompanying
 *   file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
 */

#if !defined(OMD_PARSER_BLOCK_DEF_HPP)
#define OMD_PARSER_BLOCK_DEF_HPP

#include "block.hpp"
#include <algorithm>
#include <boost/spirit/include/phoenix_core.hpp>
#include <boost/spirit/include/phoenix_container.hpp>
#include <boost/spirit/include/phoenix_statement.hpp>
#include <boost/spirit/include/phoenix_operator.hpp>

// For debugging:
#if defined(BOOST_SPIRIT_DEBUG)
#define PRINT_INDENT \
[ std::cerr << phx::val("\n============================") << get_indent << std::endl ]
#else
#define PRINT_INDENT
#endif

namespace omd { namespace yaml { namespace parser
{
    namespace detail
    {
        struct update_indent
        {
            template <typename, typename, typename>
            struct result { typedef void type; };

            template <typename Range>
            void operator()(
                Range const& rng,                   //  <-- where we are now
                int& current_indent,                //  <-- the current indent position
                bool& pass                          //  <-- set to false to fail parsing
            ) const
            {
                int pos = rng.begin().get_position().column;
                if (pos >= current_indent)
                    current_indent = pos;
                else
                    pass = false;
            }
        };

        struct check_indent
        {
            template <typename, typename, typename>
            struct result { typedef void type; };

            template <typename Range>
            void operator()(
                Range const& rng,                   //  <-- where we are now
                int const& current_indent,          //  <-- the current indent position
                bool& pass                          //  <-- set to false to fail parsing
            ) const
            {
                int pos = rng.begin().get_position().column;
                if (pos < current_indent)
                    pass = false;
            }
        };

        struct fold_line
        {
            template <typename, typename, typename, typename>
            struct result { typedef void type; };

            template <typename String, typename Range>
            void operator()(
                String& result_,
                Range const& rng,
                char indicator,
                bool different_indentation
            ) const
            {
                if (rng.empty())
                    return;

                typedef typename std::iterator_traits<typename Range::iterator>::difference_type distance_t;

                distance_t n = std::distance(rng.begin(), rng.end());

                // Don't fold the previous lines if the next line has a different indentation
                if ((indicator == '>') && different_indentation)
                {
                    for (distance_t i = 0; i != n; ++i)
                        result_ += '\n';
                    return;
                }

                if ((indicator == '>') && (n == 1))
                    result_ += ' ';
                else
                    result_ += '\n';
            }
        };

        struct chomp_string
        {
            template <typename, typename>
            struct result { typedef void type; };

            template <typename String>
            void operator()(String& result_, char indicator) const
            {
                if (indicator == '-' || indicator == 0)
                {
                    std::size_t pos = result_.size();
                    for (; pos != 0; --pos)
                    {
                        if (result_[pos-1] != '\n')
                            break;
                    }
                    if (pos < result_.size())
                    {
                        if (indicator == '-')
                            result_.erase(pos);
                        else
                            result_.erase(pos+1);
                    }
                }
            }
        };
    }

    template <typename Iterator>
    block<Iterator>::block(std::string const& source_file)
      : block::base_type(start),
        flow_g(current_indent, anchors),
        current_indent(-1),
        error_handler(error_handler_t(source_file))
    {
        namespace phx = boost::phoenix;
        using boost::spirit::qi::copy;
        phx::function<detail::update_indent> update_indent;
        phx::function<detail::check_indent> check_indent;
        phx::function<detail::fold_line> fold_line;
        phx::function<detail::chomp_string> chomp_string;
        phx::function<qi::symbols<char>::adder> add_anchor(anchors.add);

        qi::skip_type skip;
        auto space = ws.start.alias();

        qi::_val_type _val;
        qi::_1_type _1;
        qi::_2_type _2;
        qi::_3_type _3;
        qi::_4_type _4;
        qi::_a_type _a;
        qi::_b_type _b;
        qi::_c_type _c;
        qi::char_type char_;

        qi::repeat_type repeat;
        qi::omit_type omit;
        qi::_pass_type _pass;
        qi::eps_type eps;
        qi::attr_type attr;
        qi::raw_type raw;

        qi::eol_type eol;
        qi::eoi_type eoi_;
        qi::blank_type blank;

        auto comment = copy('#' >> *(char_ - eol) >> eol);  // comments
        auto blank_eol = copy(*blank >> (comment | eol));   // empty until eol

        auto map_key = copy(
            skip(space)[flow_g.scalar_value.map_key]
            );

        // no-skip version without strings
        auto flow_scalar_ns =
            flow_g.scalar_value.scalar_value_no_strings.alias()
            ;

        // Only for strings
        auto& flow_string_ns =
            flow_g.scalar_value.string_value
            ;

        auto get_indent =
            phx::ref(current_indent)
            ;

        auto save_indent = copy(
            eps[_a = get_indent] PRINT_INDENT
            );

        auto zero_indent = copy(
            eps[_a = get_indent, get_indent = 0] PRINT_INDENT
            );

        auto increase_indent = copy(
            eps[get_indent += 1] PRINT_INDENT
            );

        auto decrease_indent = copy(
            eps[get_indent -= 1] PRINT_INDENT
            );

        auto restore_indent = copy(
            eps[get_indent = _a] PRINT_INDENT
            );

        indent =
            *blank >> raw[eps] [update_indent(_1, get_indent, _pass)]
            ;

        skip_indent =
            *blank >> raw[eps] [check_indent(_1, get_indent, _pass)]
            ;

        auto skip_exact_indent = copy(
            repeat(get_indent-1)[blank]     // note: indent is one based!
            );

        skip_indent_child =
            *blank >> raw[eps] [check_indent(_1, get_indent + 1, _pass)]
            ;

        end_of_input =
            omit[
                -('#' >> *(char_ - eoi_))   //  allow comments at the very end
            >>  eoi_
            ];

        //  Allow newlines before scalars as long as they are properly indented.
        //  Make sure that the scalar doesn't span multiple lines by requiring
        //  the next line to be a higher level node (with lower indentation).
        //  We'll deal with strings and potentially multi-line strings separately.

        auto scalar_in_block = copy(
                omit[-(+blank_eol >> skip_indent_child)]
            >>  flow_scalar_ns
            >>  !(+blank_eol >> skip_indent_child)
            );

        //  Allow newlines before strings as long as they are properly indented
        auto string_in_block = copy(
                omit[-(+blank_eol >> skip_indent_child)]
            >>  flow_string_ns
            );

        // flow compound (arrays and maps) need no indentations. Set indent to
        // zero while parsing.
        flow_compound %=
                zero_indent
            >>  (skip(space)[flow_g.flow_start] | !restore_indent)
            >>  restore_indent
            ;

        block_node =
                compact_block
            |   indented_block
            |   flow_compound
            |   scalar_in_block
            |   string_in_block
            |   (omit[blank_eol | end_of_input]  //  If all else fails, then null_t
                  >> attr(ast::null_t()))
            ;

        anchored_block_node %=
                '&'
            >>  (+~char_(" \n\r\t,{}[]")) [ add_anchor(_1) ]
            >>  omit[blank | &eol]
            >>  block_node
            ;

        auto block_node_main = copy(
                anchored_block_node
            |   block_node
            );

        indented_block %=
                increase_indent
            >>  (blocks | !decrease_indent)
            >>  decrease_indent
            ;

        compact_block =
                !blank_eol
            >>  blocks
            ;

        auto block_main = copy(
                block_literal
            |   block_seq
            |   explicit_block_map
            |   implicit_block_map
            );

        blocks %=
                omit[*blank_eol]
            >>  save_indent
            >>  (block_main | !restore_indent)
            >>  restore_indent
            ;

        start %=
                blocks

                // YAML allows bare top-level scalars. Allow this, but set
                // the indent to 1 to require unquoted strings to be indented,
                // otherwise, unquoted strings will gobble up too much in
                // its path.
            |   eps[get_indent = 1] >> skip(space)[flow_g.scalar_value]
            ;

        auto start_indent = copy(
            omit[indent] PRINT_INDENT
            );

        auto block_literal_first_line = copy(
                raw[*eol]           [ fold_line(_val, _1, _a, false) ]
            >>  start_indent                                //  Get first line indent
            >>  +(char_ - eol)      [ _val += _1 ]          //  Get the line
            );

        // This rule checks for blank lines and sets local _c to true or false
        // depending on whether the succeeding line has a different indentation or not
        auto block_literal_blank_lines = copy(
            raw[(*eol)
                >> &-(skip_exact_indent >> blank)     [ _c = true ]
                >> -(!skip_exact_indent)              [ _c = true ]
            ][ fold_line(_val, _1, _a, _c) ]
            );

        // This rule checks if the current line is indented or not and
        // sets local _c accordingly
        auto block_literal_indented_line = copy(
            &(blank[ _c = true ] | eps[ _c = false ])
            );

        auto block_literal_line = copy(
                block_literal_blank_lines
            >>  skip_exact_indent                           //  Indent get_indent spaces
            >>  block_literal_indented_line
            >>  +(char_ - eol)      [ _val += _1 ]          //  Get the line
            );

        block_literal =
                (*blank)            [ _c = false, _b = 0 ]  //  initialize locals
            >>  char_("|>")         [ _a = _1 ]             //  get indicator in local _a
            >>  -char_("+-")        [ _b = _1 ]             //  get the (optional) chomping indicator
            >>  *blank >> blank_eol
            >>  block_literal_first_line
            >>  *block_literal_line
            >>  eps                 [ chomp_string(_val, _b) ]
            ;

        auto block_seq_indicator = copy(                    //  Lookahead and see if we have a
            &(start_indent >> '-' >> (blank | eol))         //  sequence indicator.
            );

        block_seq =
                omit[block_seq_indicator]
            >>  +block_seq_entry                            //  Get the entries
            ;

        block_seq_entry =
                omit[*blank_eol]                            //  Ignore blank lines
            >>  omit[skip_indent]                           //  Indent get_indent spaces
            >>  omit['-' >> (blank | &eol)]                 //  Get the sequence indicator '-'
            >>  block_node_main                             //  Get the entry
            ;

        auto implicit_block_map_indicator = copy(           //  Lookahead and see if we have an
            &(  start_indent                                //  implicit map indicator.
            >>  map_key
            >>  skip(space)[':']
            ));

        implicit_block_map =
                omit[implicit_block_map_indicator]
            >>  +block_map_entry                            //  Get the entries
            ;

        auto explicit_block_map_indicator = copy(           //  Lookahead and see if we have an
            &(start_indent >> '?' >> (blank | eol))         //  explicit map indicator.
            );

        explicit_block_map =
                omit[explicit_block_map_indicator]
            >>  +block_map_entry                            //  Get the entries
            ;

        block_map_entry =
                explicit_block_map_entry
            |   implicit_block_map_entry
            ;

        implicit_block_map_entry =
                omit[*blank_eol]                            //  Ignore blank lines
            >>  omit[skip_indent]                           //  Indent get_indent spaces
            >>  map_key                                     //  Get the key
            >>  omit[skip(space)[':']]                      //  Get the map indicator ':'
            >>  omit[*blank]                                //  Ignore blank spaces
            >>  block_node_main                             //  Get the value
            ;

        explicit_block_map_entry =
                omit[*blank_eol]                            //  Ignore blank lines
            >>  omit[skip_indent]                           //  Indent get_indent spaces
            >>  omit['?' >> (blank | &eol)]                 //  Get the map-key indicator '?'
            >>  map_key                                     //  Get the key

            >>  omit[*blank_eol]                            //  Ignore blank lines
            >>  omit[skip_indent]                           //  Indent get_indent spaces
            >>  omit[':' >> (blank | &eol)]                 //  Get the map-value indicator ':'
            >>  block_node_main                             //  Get the value
            ;

        BOOST_SPIRIT_DEBUG_NODES(
            (end_of_input)
            (block_node)
            (anchored_block_node)
            (indented_block)
            (compact_block)
            (blocks)
            (block_literal)
            (block_seq)
            (block_seq_entry)
            (implicit_block_map)
            (implicit_block_map_entry)
            (block_map_entry)
            (explicit_block_map)
            (explicit_block_map_entry)
            (indent)
        );

        qi::on_error<qi::fail>(blocks, error_handler(_1, _2, _3, _4));
    }
}}}

#endif
