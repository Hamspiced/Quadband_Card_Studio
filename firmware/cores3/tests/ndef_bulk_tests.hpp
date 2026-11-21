static void ndef_bulk_tests() {
    auto bytes = [](const Mock& p) {
        Bytes out;
        for (unsigned b = 0; b < (p.card.classic() ? 64 : p.card.units()-2); ++b)
            out.insert(out.end(),p.data[b].begin(),p.data[b].begin()+p.card.unit_size());
        return out;
    };
    auto blank = [](HfFamily family) {
        Mock p(family);
        if (p.card.classic()) {
            p.card.sak = 8; p.card.atqa = {{0x44,0}};
            for (unsigned b = 3; b < 64; b += 4) std::fill(p.data[b].begin()+10,p.data[b].end(),255);
        } else p.data[3].fill(0);
        return p;
    };
    for (auto family : {HfFamily::Classic1K,HfFamily::Ntag213,HfFamily::Ntag215,HfFamily::Ntag216}) {
        auto p = blank(family); const auto original = p.data;
        auto snapshot = bytes(p);
        assert(p.format_ndef(p.card,2,snapshot) == INVALID_ARGUMENT && !p.writes);
        auto stale = snapshot; stale[0] ^= 1;
        assert(p.format_ndef(p.card,0,stale) == STALE_DATA && !p.writes);
        p.fail_read = p.card.classic() ? 63 : int(p.card.units()-3);
        assert(p.format_ndef(p.card,0,snapshot) == AUTH_FAILED && !p.writes);
        p.fail_read = -1;
        assert(p.format_ndef(p.card,0,snapshot) == OK && p.writes);
        unsigned mutations = p.writes;
        assert(p.data[0] == original[0] && p.data[4][0] == 3 && p.data[4][1] == 0 && p.data[4][2] == 0xfe);
        if (p.card.classic()) {
            assert(p.write_units.back() == 3 && p.write_units[p.write_units.size()-2] == 2 &&
                   p.write_units[p.write_units.size()-3] == 1);
            assert(p.data[3][9] == 0xc1 && p.data[7][9] == 0x40);
            Bytes mad(p.data[1].begin(),p.data[1].end()); mad.insert(mad.end(),p.data[2].begin(),p.data[2].end());
            assert(mad[0] == crc(mad));
            assert(mad[1] == 1);
            for (unsigned s = 1; s < 16; ++s) assert(mad[s*2] == 3 && mad[s*2+1] == 0xe1);
        } else {
            assert(p.write_units.back() == 3);
            unsigned cc = family == HfFamily::Ntag213 ? 18 : family == HfFamily::Ntag215 ? 62 : 109;
            assert((p.data[3] == std::array<uint8_t,16>{{0xe1,0x10,uint8_t(cc),0}}));
            for (unsigned b = 4+cc*2; b < p.card.units(); ++b) assert(p.data[b] == original[b]);
        }
        for (unsigned i = 1; i <= mutations; ++i) {
            auto failing = blank(family); failing.fail_write = i;
            assert(failing.format_ndef(failing.card,0,bytes(failing)) == WRITE_UNVERIFIED);
            assert(failing.writes == i);
        }
        p = blank(family); p.cancel_after_write = true;
        assert(p.format_ndef(p.card,0,bytes(p)) == WRITE_UNVERIFIED && p.writes == 1);
        p = blank(family); p.data[4][0] = 0x55;
        assert(p.format_ndef(p.card,0,bytes(p)) == PERMISSION_DENIED && !p.writes);
        assert(p.format_ndef(p.card,1,bytes(p)) == OK);
        if (!p.card.classic()) {
            for (unsigned bad : {2u,3u,p.card.units()-5,p.card.units()-4}) {
                p = blank(family); p.data[bad][bad == p.card.units()-4 ? 3 : 0] = 1;
                if (bad == 2) p.data[2][2] = 1;
                assert(p.format_ndef(p.card,1,bytes(p)) == PERMISSION_DENIED && !p.writes);
            }
            p = blank(family); p.card.version[6] ^= 1;
            assert(p.format_ndef(p.card,0,bytes(p)) == UNSUPPORTED && !p.writes);
        } else {
            for (unsigned gpb : {0xc1u,0xc2u}) {
                p = blank(family); p.data[3][9] = uint8_t(gpb);
                assert(p.format_ndef(p.card,1,bytes(p)) == PERMISSION_DENIED && !p.writes);
            }
            for (unsigned b : {3u,63u}) {
                p = blank(family); p.data[b][10] = 0;
                assert(p.format_ndef(p.card,1,bytes(p)) == PERMISSION_DENIED && !p.writes);
            }
        }
    }
    // Inject cancellation at every physical policy boundary, including after
    // data/trailer transitions and during final canonical-key verification.
    auto boundary_run = [&](unsigned fail_at, unsigned& calls, unsigned& writes) {
        auto p = blank(HfFamily::Classic1K); auto expected = bytes(p);
        FormatIO io;
        auto check = [&]() { return ++calls == fail_at ? CANCELLED : OK; };
        io.guard = check;
        io.read = [&](unsigned b,const ClassicKey& key,Bytes& out) {
            auto status = check(); if (status != OK) return status;
            if (b >= 4 && p.data[trailer(b)][9] == 0x40)
                assert((key.bytes == std::array<uint8_t,6>{{0xd3,0xf7,0xd3,0xf7,0xd3,0xf7}}));
            out.assign(p.data[b].begin(),p.data[b].end()); return OK;
        };
        io.write = [&](unsigned b,const ClassicKey&,const Bytes& value) {
            auto status = check(); ++writes;
            if (status != OK) return status;
            std::copy(value.begin(),value.end(),p.data[b].begin()); return OK;
        };
        return canonical_format(p.card,0,expected,io);
    };
    unsigned calls = 0, writes = 0;
    assert(boundary_run(0,calls,writes) == OK);
    for (unsigned i = 1; i <= calls; ++i) {
        unsigned seen = 0, mutations = 0; auto status = boundary_run(i,seen,mutations);
        assert(status == (mutations ? WRITE_UNVERIFIED : CANCELLED));
    }
    for (auto family : {HfFamily::Classic1K,HfFamily::Ntag213}) {
        auto p = blank(family); const auto expected = bytes(p);
        FormatIO io;
        io.guard = [] { return OK; };
        io.read = [&](unsigned b,const ClassicKey&,Bytes& out) {
            out.assign(p.data[b].begin(),p.data[b].begin()+p.card.unit_size()); return OK;
        };
        io.write = [&](unsigned b,const ClassicKey&,const Bytes& value) {
            ++p.writes; std::copy(value.begin(),value.end(),p.data[b].begin());
            if (p.card.classic()) p.data[7][6] ^= 1;
            else p.data[p.card.units()-4][3] = 4;
            return OK;
        };
        assert(canonical_format(p.card,0,expected,io) == WRITE_UNVERIFIED && p.writes == 1);
    }
    for (auto family : {HfFamily::Ntag213,HfFamily::Ntag215,HfFamily::Ntag216}) {
        auto p = blank(family); p.data[p.card.units()-5][3] = 0xbd;
        assert(p.format_ndef(p.card,0,bytes(p)) == OK);
        if (family != HfFamily::Ntag213) {
            p = blank(family); unsigned outside = family == HfFamily::Ntag215 ? 128 : 222;
            p.data[outside][0] = 0x55;
            assert(p.format_ndef(p.card,0,bytes(p)) == PERMISSION_DENIED && !p.writes);
            assert(p.format_ndef(p.card,1,bytes(p)) == OK && p.data[outside][0] == 0x55);
        }
    }
    NdefRecord record; std::string error;
    assert(ndef_build(0,{{"text","Héllo 世界"}},record,error) == OK);
    HfSnapshot target; HfReadOptions options;
    {
        Mock hidden;
        hidden.data[4] = {{3,0,0xfe,0}};
        hidden.data[20][0] = 0x55;
        assert(hf_read(hidden,options,target).verified);
        BulkNdef empty;
        assert(empty.arm(hidden,HfFamily::Ntag213,{record},false,false) == OK);
        empty.observe(NO_CARD,hidden.gen);
        assert(empty.review(hidden,target).status == PERMISSION_DENIED && !hidden.writes);
    }
    for (auto family : {HfFamily::Classic1K,HfFamily::Ntag213,HfFamily::Ntag215,HfFamily::Ntag216}) {
        auto factory = blank(family); BulkNdef formatted_job;
        assert(hf_read(factory,options,target).verified);
        assert(formatted_job.arm(factory,family,{record},false,true) == OK);
        formatted_job.observe(NO_CARD,factory.gen);
        assert(formatted_job.review(factory,target).status == OK && !factory.writes);
        assert(formatted_job.needs_format);
        assert(formatted_job.confirm(factory).verified && formatted_job.written == 1);
        HfNdefArea area;
        assert(hf_read(factory,options,target).verified && hf_ndef_area(target,area).status == OK);
        assert(area.records == std::vector<NdefRecord>{record});
    }
    Mock p; BulkNdef job;
    assert(job.arm(p,HfFamily::Ntag213,{record,record,record,record},false,false) == INVALID_ARGUMENT);
    assert(job.arm(p,HfFamily::Classic4K,{record},false,false) == INVALID_ARGUMENT);
    assert(job.arm(p,HfFamily::Ntag213,{record,record,record},false,false) == OK);
    assert(hf_read(p,options,target).verified && p.page_reads == 3);
    assert(job.review(p,target).status == PERMISSION_DENIED && !p.writes);
    job.observe(AUTH_FAILED,p.gen); assert(job.phase == BulkNdef::Phase::Removal);
    job.observe(IO_ERROR,p.gen); assert(job.phase == BulkNdef::Phase::Removal);
    job.observe(NO_CARD,p.gen); assert(job.phase == BulkNdef::Phase::Target);
    job.observe(AUTH_FAILED,p.gen); assert(job.phase == BulkNdef::Phase::Removal);
    assert(job.review(p,target).status == PERMISSION_DENIED && !p.writes);
    job.observe(NO_CARD,p.gen); job.observe(IO_ERROR,p.gen);
    assert(job.phase == BulkNdef::Phase::Removal);
    job.observe(NO_CARD,p.gen);
    assert(job.review(p,target).status == OK && !p.writes);
    job.cancel_review(); assert(job.phase == BulkNdef::Phase::Removal);
    assert(job.confirm(p).status == PERMISSION_DENIED);
    job.observe(NO_CARD,p.gen); assert(job.review(p,target).status == OK);
    assert(job.confirm(p).verified && job.written == 1);
    unsigned mutations = p.writes;
    assert(job.confirm(p).status == PERMISSION_DENIED && p.writes == mutations);
    assert(hf_read(p,options,target).verified);
    job.observe(NO_CARD,p.gen); assert(job.review(p,target).status == OK);
    assert(job.confirm(p).verified && job.identical == 1 && p.writes == mutations);
    job.stop(); job.clear();
    assert(job.arm(p,HfFamily::Ntag215,{record},false,false) == OK);
    job.observe(NO_CARD,p.gen);
    assert(job.review(p,target).status == INVALID_ARGUMENT && p.writes == mutations);
    job.clear();
    assert(job.arm(p,HfFamily::Ntag213,{record},false,false) == OK);
    job.observe(NO_CARD,p.gen);
    assert(job.review(p,target).status == PERMISSION_DENIED && p.writes == mutations);
    job.clear();
    assert(job.arm(p,HfFamily::Ntag213,{record},true,false) == OK);
    job.observe(NO_CARD,p.gen); assert(job.review(p,target).status == OK);
    p.data[40][0] = 1;
    assert(job.confirm(p).status == STALE_DATA && p.writes == mutations);
    p = Mock(); job.clear(); assert(hf_read(p,options,target).verified);
    assert(job.arm(p,HfFamily::Ntag213,{record},false,false) == OK);
    job.observe(NO_CARD,p.gen); assert(job.review(p,target).status == OK);
    p.cancel_after_write = true;
    assert(job.confirm(p).status == WRITE_UNVERIFIED && job.failed == 1);
    job.stop(); assert(job.active() && job.phase == BulkNdef::Phase::Unverified);
    for (unsigned length : {247u,248u}) {
        assert(ndef_build(0,{{"text",std::string(length,'x')}},record,error) == OK);
        Bytes encoded; assert(ndef_encode({record},encoded,error) == OK && encoded.size() == length+7);
        assert((encoded.size()<255 ? 2 : 4) == (length == 247 ? 2 : 4));
    }
    for (unsigned payload : {138u,139u}) {
        p = Mock(); job.clear(); assert(hf_read(p,options,target).verified);
        record = NdefRecord{2,{'x'},Bytes(payload,'x'),{}};
        assert(job.arm(p,HfFamily::Ntag213,{record},false,false) == OK);
        job.observe(NO_CARD,p.gen);
        assert(job.review(p,target).status == (payload == 138 ? OK : INVALID_ARGUMENT));
    }
    for (auto family : {HfFamily::Ntag213,HfFamily::Ntag215,HfFamily::Ntag216}) {
        p = Mock(family); assert(hf_read(p,options,target).verified);
        assert(p.page_reads == (p.card.units()-2+15)/16);
    }
    for (unsigned op : {22u,23u}) {
        Frame f; f.op = uint8_t(op); f.payload = {7,4,1,2,3,4,5,6,0};
        f.payload.resize(f.payload.size()+(op == 22 ? 1024 : 172));
        assert(validate_hf(f) == OK);
        f.payload[8] = 2; assert(validate_hf(f) == INVALID_ARGUMENT);
        f.payload[8] = 0; f.payload.pop_back(); assert(validate_hf(f) == INVALID_ARGUMENT);
    }
}
