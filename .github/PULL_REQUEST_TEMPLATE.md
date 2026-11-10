## Summary

<!-- What changed, and why? One or two sentences is plenty. -->

## Related issue

<!-- Link the issue with "Fixes #123" so it closes on merge.
     Remove this section if there is no issue. -->

## Type of change

- [ ] Bug fix
- [ ] New feature
- [ ] Documentation
- [ ] Refactor (no behavior change)
- [ ] Tests
- [ ] Build / packaging

## Testing

<!-- How did you verify the change? What did you run, and on which OS? -->

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build
```

- [ ] The core test suite passes

## Checklist

- [ ] The diff stays focused on one change (no unrelated cleanups)
- [ ] Existing behavior is not broken
- [ ] Comments are only added where the logic genuinely needs them
- [ ] Docs are updated where behavior changed (README, docs/)
