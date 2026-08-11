#include "ot.hpp"

#include <cassert>
#include <iostream>
#include <string>

namespace {

collab::Operation make_insert(std::size_t pos, const std::string& text, const std::string& id,
                              std::size_t base = 0) {
    collab::Operation op;
    op.type = collab::OpType::Insert;
    op.position = pos;
    op.text = text;
    op.op_id = id;
    op.client_id = id;
    op.base_revision = base;
    return op;
}

collab::Operation make_delete(std::size_t pos, std::size_t count, const std::string& id,
                              std::size_t base = 0) {
    collab::Operation op;
    op.type = collab::OpType::Delete;
    op.position = pos;
    op.count = count;
    op.op_id = id;
    op.client_id = id;
    op.base_revision = base;
    return op;
}

void wipe(collab::DocumentSession& session) {
    collab::Operation op;
    op.type = collab::OpType::Delete;
    op.position = 0;
    op.count = session.document().size();
    op.base_revision = session.revision();
    op.op_id = "wipe";
    op.client_id = "wipe";
    session.commit(op);
}

} // namespace

int main() {
    {
        collab::Operation first = make_insert(1, "X", "a");
        collab::Operation second = make_insert(1, "Y", "b");
        auto transformed = collab::transform(second, first);
        assert(transformed.position == 2);
    }

    {
        collab::Operation committed_delete = make_delete(2, 3, "d1");
        collab::Operation incoming_insert = make_insert(4, "!", "i1");
        auto transformed = collab::transform(incoming_insert, committed_delete);
        assert(transformed.position == 2);
    }

    {
        collab::Operation committed_insert = make_insert(2, "abc", "i2");
        collab::Operation incoming_delete = make_delete(1, 3, "d2");
        auto transformed = collab::transform(incoming_delete, committed_insert);
        assert(transformed.position == 1);
        assert(transformed.count == 6);
    }

    {
        collab::DocumentSession session;
        session.commit(make_insert(0, "A", "client-1", 0));
        auto committed = session.commit(make_delete(0, 1, "client-2", 0));
        assert(committed.position == 1);
    }

    // Same-base concurrent inserts converge regardless of commit order.
    {
        collab::DocumentSession order_ab;
        collab::DocumentSession order_ba;
        wipe(order_ab);
        wipe(order_ba);

        const auto base = order_ab.revision();
        auto left = make_insert(0, "L", "op-l", base);
        auto right = make_insert(0, "R", "op-r", base);

        order_ab.commit(left);
        order_ab.commit(right);

        order_ba.commit(right);
        order_ba.commit(left);

        assert(order_ab.document() == order_ba.document());
        assert(order_ab.document() == "LR");
    }

    // Non-overlapping insert + delete converge.
    {
        collab::DocumentSession ab;
        collab::DocumentSession ba;
        wipe(ab);
        wipe(ba);
        const auto seeded_base = ab.revision();
        ab.commit(make_insert(0, "abcd", "seed", seeded_base));
        ba.commit(make_insert(0, "abcd", "seed", seeded_base));

        const auto base = ab.revision();
        auto ins = make_insert(4, "Z", "ins", base); // after text
        auto del = make_delete(0, 1, "del", base);   // remove leading 'a'

        ab.commit(ins);
        ab.commit(del);

        ba.commit(del);
        ba.commit(ins);

        assert(ab.document() == ba.document());
        assert(ab.document() == "bcdZ");
    }

    // Revision numbers are strictly increasing and assigned server-side.
    {
        collab::DocumentSession session;
        auto first = session.commit(make_insert(0, "1", "r1", 0));
        auto second = session.commit(make_insert(0, "2", "r2", first.revision));
        assert(first.revision == 1);
        assert(second.revision == 2);
        assert(!first.op_id.empty());
        assert(!second.op_id.empty());
    }

    // Operation IDs break insert/insert ties deterministically.
    {
        auto left = make_insert(0, "A", "aaa");
        auto right = make_insert(0, "B", "bbb");
        auto right_after_left = collab::transform(right, left);
        auto left_after_right = collab::transform(left, right);
        assert(right_after_left.position == 1);
        assert(left_after_right.position == 0);
    }

    std::cout << "OT tests passed\n";
}
