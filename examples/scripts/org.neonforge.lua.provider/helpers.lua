return {add = function(a, b)
    -- Check before arithmetic: Lua integer addition otherwise wraps.
    assert(not ((b > 0 and a > math.maxinteger - b) or
                (b < 0 and a < math.mininteger - b)), 'signed addition overflow')
    return a + b
end}
